/**
 * @file    tls_server.c
 * @brief   TLS on top of the W6300's hardware TCP sockets (camera_stream_https).
 *
 * See tls_server.h for the shape. Numbers this is built around, from tls_bench
 * on the same board (200 MHz, server side):
 *
 *   ECDHE-ECDSA P-256 full handshake   129 ms   (one loop stall per new client)
 *   ticket resumption                  2.9 ms   (a browser's later connections)
 *   ChaCha20-Poly1305, 16 KB frame     5.0 ms   preferred
 *   AES-128-GCM, 16 KB frame           16.1 ms  fallback
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/rand.h"

#include "wizchip_conf.h"
#include "socket.h"

#include "mbedtls/platform.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ticket.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

#include "tls_server.h"

extern const unsigned char https_dev_cert_der[];
extern const unsigned int  https_dev_cert_der_len;
extern const unsigned char https_ca_cert_der[];
extern const unsigned int  https_ca_cert_der_len;
extern const unsigned char https_dev_key_der[];
extern const unsigned int  https_dev_key_der_len;

#define TLS_SOCKETS        8         /* hardware sockets the server polls */
#define TLS_SEG_MAX        1460
#define TLS_SEND_TIMEOUT_MS 2000
/* A full handshake costs ~129 ms of CPU, a resumed one ~3 ms. Anything under
 * this is reported as a resumption; the gap is wide enough not to blur. */
#define TLS_RESUMED_BELOW_US 30000u

#define BIO_CONN_RESET     (-0x0050)  /* MBEDTLS_ERR_NET_CONN_RESET without net_sockets.h */

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    size_t i = 0;
    while (i < len) {
        uint32_t r = get_rand_32();           /* TRNG-seeded on RP2350 */
        size_t n = (len - i < 4) ? len - i : 4;
        memcpy(output + i, &r, n);
        i += n;
    }
    *olen = len;
    return 0;
}

static mbedtls_entropy_context    g_entropy;
static mbedtls_ctr_drbg_context   g_drbg;
static mbedtls_x509_crt           g_chain;      /* device cert, then CA */
static mbedtls_pk_context         g_key;
static mbedtls_ssl_ticket_context g_ticket;
static mbedtls_ssl_config         g_conf;

static const int g_suites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    0
};

typedef struct {
    bool                active;
    bool                ready;
    uint64_t            hs_cpu_us;   /* time spent inside handshake steps */
    uint64_t            hs_start_us;
    mbedtls_ssl_context ssl;
} tls_slot_t;

static tls_slot_t g_slot[TLS_SOCKETS];
static unsigned   g_active;
static uint32_t   g_full, g_resumed, g_fail, g_refused;
static uint32_t   g_last_hs_cpu_ms, g_last_hs_wall_ms;
static bool       g_tls_ok;          /* tls_server_init() completed */

static void report(const char *what, int rc)
{
    char e[96];
    mbedtls_strerror(rc, e, sizeof(e));
    printf("[tls] %s: -0x%04X %s\n", what, (unsigned)-rc, e);
}

/* ------------------------------------------------------------------ BIO -- */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    uint8_t  sn = (uint8_t)(uintptr_t)ctx;
    uint8_t  sr = getSn_SR(sn);
    uint16_t free_sz = 0;

    if (sr != SOCK_ESTABLISHED && sr != SOCK_CLOSE_WAIT) return BIO_CONN_RESET;
    getsockopt(sn, SO_SENDBUF, &free_sz);
    if (free_sz == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;

    size_t n = len;
    if (n > free_sz) n = free_sz;
    if (n > TLS_SEG_MAX) n = TLS_SEG_MAX;

    int32_t r = send(sn, (uint8_t *)buf, (uint16_t)n);
    if (r < 0)  return BIO_CONN_RESET;
    if (r == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    uint8_t  sn = (uint8_t)(uintptr_t)ctx;
    uint16_t avail = 0;

    getsockopt(sn, SO_RECVBUF, &avail);
    if (avail == 0) {
        uint8_t sr = getSn_SR(sn);
        return (sr == SOCK_ESTABLISHED) ? MBEDTLS_ERR_SSL_WANT_READ : BIO_CONN_RESET;
    }
    size_t n = len;
    if (n > avail) n = avail;
    int32_t r = recv(sn, buf, (uint16_t)n);
    if (r <= 0) return (r == 0) ? MBEDTLS_ERR_SSL_WANT_READ : BIO_CONN_RESET;
    return (int)r;
}

/* ---------------------------------------------------------------- setup -- */
bool tls_server_init(void)
{
    int rc;

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                               (const unsigned char *)"camera_stream_https", 19);
    if (rc) { report("drbg seed", rc); return false; }

    mbedtls_x509_crt_init(&g_chain);
    rc = mbedtls_x509_crt_parse_der(&g_chain, https_dev_cert_der, https_dev_cert_der_len);
    if (!rc) rc = mbedtls_x509_crt_parse_der(&g_chain, https_ca_cert_der, https_ca_cert_der_len);
    if (rc) { report("certificate parse", rc); return false; }

    mbedtls_pk_init(&g_key);
    rc = mbedtls_pk_parse_key(&g_key, https_dev_key_der, https_dev_key_der_len, NULL, 0,
                              mbedtls_ctr_drbg_random, &g_drbg);
    if (rc) { report("key parse", rc); return false; }

    mbedtls_ssl_ticket_init(&g_ticket);
    rc = mbedtls_ssl_ticket_setup(&g_ticket, mbedtls_ctr_drbg_random, &g_drbg,
                                  MBEDTLS_CIPHER_AES_128_GCM, 86400);
    if (rc) { report("ticket setup", rc); return false; }

    mbedtls_ssl_config_init(&g_conf);
    rc = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) { report("config defaults", rc); return false; }
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_ciphersuites(&g_conf, g_suites);
    /* Our order, not the browser's: browsers on PCs with AES instructions list
     * AES-GCM first, which is 3x slower here. */
    mbedtls_ssl_conf_preference_order(&g_conf, MBEDTLS_SSL_SRV_CIPHERSUITE_ORDER_SERVER);
    rc = mbedtls_ssl_conf_own_cert(&g_conf, &g_chain, &g_key);
    if (rc) { report("own cert", rc); return false; }
    mbedtls_ssl_conf_session_tickets_cb(&g_conf, mbedtls_ssl_ticket_write,
                                        mbedtls_ssl_ticket_parse, &g_ticket);

    g_tls_ok = true;
    printf("[tls] ready: ECDHE-ECDSA P-256, ChaCha20-Poly1305 then AES-128-GCM,"
           " tickets on, max %u connections\n", (unsigned)TLS_MAX_CONN);
    return true;
}

/* ------------------------------------------------------------ lifecycle -- */
int tls_random(void *buf, size_t len)
{
    return mbedtls_ctr_drbg_random(&g_drbg, buf, len);
}

bool tls_active(uint8_t sn) { return sn < TLS_SOCKETS && g_slot[sn].active; }
bool tls_ready(uint8_t sn)  { return sn < TLS_SOCKETS && g_slot[sn].ready; }

bool tls_accept(uint8_t sn)
{
    if (sn >= TLS_SOCKETS) return false;
    tls_slot_t *s = &g_slot[sn];
    if (s->active) return true;
    if (!g_tls_ok) {                    /* init failed: refuse, do not half-serve */
        g_refused++;
        return false;
    }
    if (g_active >= TLS_MAX_CONN) {
        g_refused++;
        printf("[tls] s%u refused: %u connections already (max %u)\n",
               sn, g_active, (unsigned)TLS_MAX_CONN);
        return false;
    }
    mbedtls_ssl_init(&s->ssl);
    int rc = mbedtls_ssl_setup(&s->ssl, &g_conf);
    if (rc) {
        report("ssl setup (out of heap?)", rc);
        mbedtls_ssl_free(&s->ssl);
        g_refused++;
        return false;
    }
    mbedtls_ssl_set_bio(&s->ssl, (void *)(uintptr_t)sn, bio_send, bio_recv, NULL);
    s->active = true;
    s->ready = false;
    s->hs_cpu_us = 0;
    s->hs_start_us = time_us_64();
    g_active++;
    return true;
}

int tls_handshake_step(uint8_t sn)
{
    tls_slot_t *s = &g_slot[sn];
    if (!s->active) return -1;
    if (s->ready) return 1;

    uint64_t t0 = time_us_64();
    int rc = mbedtls_ssl_handshake(&s->ssl);
    s->hs_cpu_us += time_us_64() - t0;

    if (rc == 0) {
        s->ready = true;
        bool resumed = s->hs_cpu_us < TLS_RESUMED_BELOW_US;
        if (resumed) g_resumed++; else g_full++;
        g_last_hs_cpu_ms  = (uint32_t)(s->hs_cpu_us / 1000);
        g_last_hs_wall_ms = (uint32_t)((time_us_64() - s->hs_start_us) / 1000);
        printf("[tls] s%u handshake %s: cpu %lu ms, wall %lu ms, %s\n", sn,
               resumed ? "resumed" : "full",
               (unsigned long)g_last_hs_cpu_ms, (unsigned long)g_last_hs_wall_ms,
               mbedtls_ssl_get_ciphersuite(&s->ssl));
        return 1;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    g_fail++;
    report("handshake failed (browser not trusting the CA yet?)", rc);
    return -1;
}

int32_t tls_send_all(uint8_t sn, const uint8_t *buf, uint32_t len)
{
    tls_slot_t *s = &g_slot[sn];
    if (!s->active || !s->ready) return -1;

    uint32_t        sent = 0;
    absolute_time_t deadline = make_timeout_time_ms(TLS_SEND_TIMEOUT_MS);

    while (sent < len) {
        int rc = mbedtls_ssl_write(&s->ssl, buf + sent, len - sent);
        if (rc > 0) {
            sent += (uint32_t)rc;
            deadline = make_timeout_time_ms(TLS_SEND_TIMEOUT_MS);
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) return -2;
            continue;
        }
        return -1;
    }
    return (int32_t)sent;
}

int32_t tls_recv(uint8_t sn, uint8_t *buf, uint32_t cap)
{
    tls_slot_t *s = &g_slot[sn];
    if (!s->active || !s->ready) return -1;
    int rc = mbedtls_ssl_read(&s->ssl, buf, cap);
    if (rc > 0) return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    return -1;      /* close_notify (0) or an error: either way this request is over */
}

void tls_close(uint8_t sn)
{
    if (sn >= TLS_SOCKETS) return;
    tls_slot_t *s = &g_slot[sn];
    if (!s->active) return;
    if (s->ready) {
        (void)mbedtls_ssl_close_notify(&s->ssl);   /* best effort, may be WANT_WRITE */
    }
    mbedtls_ssl_free(&s->ssl);
    s->active = false;
    s->ready = false;
    if (g_active) g_active--;
}

int tls_status_json(char *buf, size_t cap)
{
    return snprintf(buf, cap,
                    "\"tls_conns\":%u,\"tls_full\":%lu,\"tls_resumed\":%lu,"
                    "\"tls_fail\":%lu,\"tls_refused\":%lu,\"tls_hs_ms\":%lu,",
                    g_active, (unsigned long)g_full, (unsigned long)g_resumed,
                    (unsigned long)g_fail, (unsigned long)g_refused,
                    (unsigned long)g_last_hs_cpu_ms);
}
