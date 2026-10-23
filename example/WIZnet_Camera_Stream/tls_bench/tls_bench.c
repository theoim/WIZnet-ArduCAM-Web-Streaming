/**
 * @file    tls_bench.c
 * @brief   tls_bench: what HTTPS would cost the camera, measured on the RP2350.
 *
 * No network. A TLS server and a TLS client run on the same core, joined by two
 * in-memory pipes, and only the time spent inside the SERVER's mbedTLS calls is
 * counted - that is the work the camera would do. The client's share is
 * reported separately so the total is visible but never mixed in.
 *
 * Measured, in order:
 *   1. raw AEAD throughput: AES-128-GCM and ChaCha20-Poly1305 on 16 KB and
 *      32 KB buffers (a VGA frame is 16-17 KB, 720p 35-37 KB)
 *   2. full handshake, ECDHE-ECDSA P-256, server side, each suite
 *   3. resumed handshake from a session ticket, server side - what a browser's
 *      second connection would cost
 *   4. sending one frame through an established session: mbedtls_ssl_write of
 *      16 KB and 32 KB, i.e. encryption plus record framing
 *   5. heap peak per side, through counted calloc/free
 *
 * Baselines the numbers are for (cam/README.md, 2026-10-22):
 *   OV2640 800x600   17.1 fps  24-28 KB  period ~58 ms
 *   LH     640x480   20-23 fps 16-17 KB  period ~45 ms
 *   LH     1280x720  28.1 fps  35-37 KB  period ~35 ms
 *
 * Runs at 200 MHz like the streaming images. The OPC UA crypto numbers were at
 * 150 MHz - do not compare the two without scaling.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/clocks.h"

#include "mbedtls/platform.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/gcm.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ticket.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

extern const unsigned char bench_cert_der[];
extern const unsigned int  bench_cert_der_len;
extern const unsigned char bench_key_der[];
extern const unsigned int  bench_key_der_len;

/* ------------------------------------------------------------ entropy ---- */
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

/* ------------------------------------------------------ counted heap ---- */
/* Each block carries its size and its owner, so a free made during the other
 * side's call is still charged back to the side that allocated it. */
enum { OWN_OTHER = 0, OWN_SERVER = 1, OWN_CLIENT = 2 };
static int    g_owner = OWN_OTHER;
static size_t g_cur[3], g_peak[3];

typedef struct { uint32_t size; uint32_t owner; } blk_t;

static void *bench_calloc(size_t n, size_t sz)
{
    size_t bytes = n * sz;
    blk_t *b = calloc(1, sizeof(blk_t) + bytes);
    if (!b) return NULL;
    b->size = (uint32_t)bytes;
    b->owner = (uint32_t)g_owner;
    g_cur[g_owner] += bytes;
    if (g_cur[g_owner] > g_peak[g_owner]) g_peak[g_owner] = g_cur[g_owner];
    return b + 1;
}

static void bench_free(void *p)
{
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    g_cur[b->owner] -= b->size;
    free(b);
}

static void heap_reset_peaks(void)
{
    for (int i = 0; i < 3; i++) g_peak[i] = g_cur[i];
}

/* ------------------------------------------------------- memory pipes ---- */
#define PIPE_CAP  (40 * 1024)

typedef struct {
    uint8_t buf[PIPE_CAP];
    size_t  head, len;
} pipe_t;

static pipe_t g_c2s, g_s2c;

static void pipe_clear(pipe_t *p) { p->head = 0; p->len = 0; }

static int pipe_send(void *ctx, const unsigned char *d, size_t n)
{
    pipe_t *p = ctx;
    size_t space = PIPE_CAP - p->len;
    if (space == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (n > space) n = space;
    for (size_t i = 0; i < n; i++) {
        p->buf[(p->head + p->len + i) % PIPE_CAP] = d[i];
    }
    p->len += n;
    return (int)n;
}

static int pipe_recv(void *ctx, unsigned char *d, size_t n)
{
    pipe_t *p = ctx;
    if (p->len == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n > p->len) n = p->len;
    for (size_t i = 0; i < n; i++) {
        d[i] = p->buf[(p->head + i) % PIPE_CAP];
    }
    p->head = (p->head + n) % PIPE_CAP;
    p->len -= n;
    return (int)n;
}

/* ------------------------------------------------------------- shared ---- */
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt         g_cert;
static mbedtls_pk_context       g_key;
static mbedtls_ssl_ticket_context g_ticket;

static uint8_t g_frame[32 * 1024];
static uint8_t g_sink[17 * 1024];

static void die(const char *what, int rc)
{
    char e[100];
    mbedtls_strerror(rc, e, sizeof(e));
    printf("FAIL %s: -0x%04X %s\n", what, (unsigned)-rc, e);
    while (1) sleep_ms(1000);
}

/* ------------------------------------------------------ 1. raw AEAD ---- */
static void bench_aead(void)
{
    static const uint8_t key[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    uint8_t iv[12] = {0}, tag[16];
    static uint8_t out[32 * 1024];
    const size_t sizes[2] = {16 * 1024, 32 * 1024};
    const int reps = 20;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);

    mbedtls_chachapoly_context cp;
    mbedtls_chachapoly_init(&cp);
    mbedtls_chachapoly_setkey(&cp, key);

    printf("\n[1] raw AEAD, %d reps each\n", reps);
    for (int s = 0; s < 2; s++) {
        size_t n = sizes[s];
        uint64_t t0 = time_us_64();
        for (int i = 0; i < reps; i++) {
            iv[0] = (uint8_t)i;
            mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, iv, 12, NULL, 0,
                                      g_frame, out, 16, tag);
        }
        uint32_t us_gcm = (uint32_t)((time_us_64() - t0) / reps);

        t0 = time_us_64();
        for (int i = 0; i < reps; i++) {
            iv[0] = (uint8_t)i;
            mbedtls_chachapoly_encrypt_and_tag(&cp, n, iv, NULL, 0, g_frame, out, tag);
        }
        uint32_t us_cp = (uint32_t)((time_us_64() - t0) / reps);

        printf("  %2u KB  AES-128-GCM %6lu us (%5.2f MB/s)   ChaCha20-Poly1305 %6lu us (%5.2f MB/s)\n",
               (unsigned)(n / 1024),
               (unsigned long)us_gcm, (double)n / us_gcm,
               (unsigned long)us_cp, (double)n / us_cp);
    }
    mbedtls_gcm_free(&gcm);
    mbedtls_chachapoly_free(&cp);
}

/* --------------------------------------------- 2-5. TLS through pipes ---- */
typedef struct {
    mbedtls_ssl_context ssl;
    uint64_t us;
} side_t;

static void setup_conf(mbedtls_ssl_config *conf, int endpoint, const int *suites)
{
    mbedtls_ssl_config_init(conf);
    int rc = mbedtls_ssl_config_defaults(conf, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) die("config_defaults", rc);
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_ciphersuites(conf, suites);
    if (endpoint == MBEDTLS_SSL_IS_SERVER) {
        rc = mbedtls_ssl_conf_own_cert(conf, &g_cert, &g_key);
        if (rc) die("own_cert", rc);
        mbedtls_ssl_conf_session_tickets_cb(conf, mbedtls_ssl_ticket_write,
                                            mbedtls_ssl_ticket_parse, &g_ticket);
    } else {
        /* The client is only the other end of the pipe; its certificate check
         * is not the camera's cost. */
        mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_session_tickets(conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
    }
}

static void step(side_t *s, int owner, int *done, const char *who)
{
    if (*done) return;
    g_owner = owner;
    uint64_t t0 = time_us_64();
    int rc = mbedtls_ssl_handshake(&s->ssl);
    s->us += time_us_64() - t0;
    g_owner = OWN_OTHER;
    if (rc == 0) *done = 1;
    else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) die(who, rc);
}

static void handshake(side_t *srv, side_t *cli)
{
    int sd = 0, cd = 0;
    srv->us = cli->us = 0;
    while (!(sd && cd)) {
        step(cli, OWN_CLIENT, &cd, "client handshake");
        step(srv, OWN_SERVER, &sd, "server handshake");
    }
}

/* Two pipes, each with one writer: the server sends on s2c and receives on
 * c2s, the client the other way round. mbedtls_ssl_set_bio takes one context
 * for both directions, so each side gets a small struct naming its two pipes. */
typedef struct { pipe_t *tx, *rx; } duplex_t;
static duplex_t g_srv_io = { &g_s2c, &g_c2s };
static duplex_t g_cli_io = { &g_c2s, &g_s2c };

static int dx_send(void *ctx, const unsigned char *d, size_t n)
{
    return pipe_send(((duplex_t *)ctx)->tx, d, n);
}

static int dx_recv(void *ctx, unsigned char *d, size_t n)
{
    return pipe_recv(((duplex_t *)ctx)->rx, d, n);
}

static void make_pair(side_t *srv, side_t *cli,
                      mbedtls_ssl_config *sconf, mbedtls_ssl_config *cconf)
{
    pipe_clear(&g_c2s);
    pipe_clear(&g_s2c);
    g_owner = OWN_SERVER;
    mbedtls_ssl_init(&srv->ssl);
    int rc = mbedtls_ssl_setup(&srv->ssl, sconf);
    if (rc) die("server setup", rc);
    mbedtls_ssl_set_bio(&srv->ssl, &g_srv_io, dx_send, dx_recv, NULL);
    g_owner = OWN_CLIENT;
    mbedtls_ssl_init(&cli->ssl);
    rc = mbedtls_ssl_setup(&cli->ssl, cconf);
    if (rc) die("client setup", rc);
    mbedtls_ssl_set_bio(&cli->ssl, &g_cli_io, dx_send, dx_recv, NULL);
    mbedtls_ssl_set_hostname(&cli->ssl, "camera-stream-tls-bench");
    g_owner = OWN_OTHER;
}

static void free_pair(side_t *srv, side_t *cli)
{
    g_owner = OWN_SERVER; mbedtls_ssl_free(&srv->ssl);
    g_owner = OWN_CLIENT; mbedtls_ssl_free(&cli->ssl);
    g_owner = OWN_OTHER;
}

/* Server sends n bytes; the client drains as it goes. Server time only. */
static uint32_t send_frame(side_t *srv, side_t *cli, size_t n)
{
    size_t sent = 0;
    uint64_t us = 0;
    while (sent < n) {
        g_owner = OWN_SERVER;
        uint64_t t0 = time_us_64();
        int rc = mbedtls_ssl_write(&srv->ssl, g_frame + sent, n - sent);
        us += time_us_64() - t0;
        g_owner = OWN_OTHER;
        if (rc > 0) sent += (size_t)rc;
        else if (rc != MBEDTLS_ERR_SSL_WANT_WRITE) die("ssl_write", rc);

        g_owner = OWN_CLIENT;
        int r;
        do { r = mbedtls_ssl_read(&cli->ssl, g_sink, sizeof(g_sink)); } while (r > 0);
        g_owner = OWN_OTHER;
        if (r != MBEDTLS_ERR_SSL_WANT_READ) die("ssl_read", r);
    }
    return (uint32_t)us;
}

static void bench_suite(const char *name, int suite_id)
{
    const int suites[2] = { suite_id, 0 };
    mbedtls_ssl_config sconf, cconf;
    side_t srv, cli;
    const int reps = 5;

    setup_conf(&sconf, MBEDTLS_SSL_IS_SERVER, suites);
    setup_conf(&cconf, MBEDTLS_SSL_IS_CLIENT, suites);

    printf("\n[%s]\n", name);

    /* 2. full handshakes */
    uint64_t full_srv = 0, full_cli = 0;
    size_t   hs_peak = 0;
    mbedtls_ssl_session saved;
    mbedtls_ssl_session_init(&saved);
    for (int i = 0; i < reps; i++) {
        heap_reset_peaks();
        make_pair(&srv, &cli, &sconf, &cconf);
        handshake(&srv, &cli);
        full_srv += srv.us;
        full_cli += cli.us;
        if (g_peak[OWN_SERVER] > hs_peak) hs_peak = g_peak[OWN_SERVER];
        if (i == reps - 1) {
            int rc = mbedtls_ssl_get_session(&cli.ssl, &saved);
            if (rc) die("get_session", rc);
        }
        free_pair(&srv, &cli);
    }
    printf("  full handshake      server %6lu us   (client %6lu us)   server heap peak %u B\n",
           (unsigned long)(full_srv / reps), (unsigned long)(full_cli / reps), (unsigned)hs_peak);

    /* 3. resumed handshakes from the ticket */
    uint64_t res_srv = 0, res_cli = 0;
    for (int i = 0; i < reps; i++) {
        make_pair(&srv, &cli, &sconf, &cconf);
        int rc = mbedtls_ssl_set_session(&cli.ssl, &saved);
        if (rc) die("set_session", rc);
        handshake(&srv, &cli);
        res_srv += srv.us;
        res_cli += cli.us;
        free_pair(&srv, &cli);
    }
    printf("  resumed (ticket)    server %6lu us   (client %6lu us)\n",
           (unsigned long)(res_srv / reps), (unsigned long)(res_cli / reps));

    /* 4. frames through an established session */
    make_pair(&srv, &cli, &sconf, &cconf);
    handshake(&srv, &cli);
    heap_reset_peaks();
    const size_t sizes[2] = {16 * 1024, 32 * 1024};
    for (int s = 0; s < 2; s++) {
        uint64_t total = 0;
        const int frames = 20;
        for (int i = 0; i < frames; i++) total += send_frame(&srv, &cli, sizes[s]);
        uint32_t us = (uint32_t)(total / frames);
        printf("  ssl_write %2u KB     server %6lu us per frame (%5.2f MB/s)\n",
               (unsigned)(sizes[s] / 1024), (unsigned long)us, (double)sizes[s] / us);
    }
    printf("  heap while streaming: server now %u B, peak %u B; client now %u B\n",
           (unsigned)g_cur[OWN_SERVER], (unsigned)g_peak[OWN_SERVER], (unsigned)g_cur[OWN_CLIENT]);
    free_pair(&srv, &cli);

    mbedtls_ssl_session_free(&saved);
    mbedtls_ssl_config_free(&sconf);
    mbedtls_ssl_config_free(&cconf);
}

int main(void)
{
    set_sys_clock_khz(200000, true);
    stdio_init_all();
    for (int i = 0; i < 50 && !stdio_usb_connected(); i++) sleep_ms(100);

    printf("\n=== tls_bench  (no network) ===\n");
    printf("build %s %s  sys %lu MHz  mbedTLS " MBEDTLS_VERSION_STRING "\n",
           CAM_BUILD_GIT, CAM_BUILD_UTC, (unsigned long)(clock_get_hz(clk_sys) / 1000000));

    mbedtls_platform_set_calloc_free(bench_calloc, bench_free);
    for (size_t i = 0; i < sizeof(g_frame); i++) g_frame[i] = (uint8_t)(i * 31u + 7u);

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                   (const unsigned char *)"tls_bench", 9);
    if (rc) die("drbg seed", rc);

    mbedtls_x509_crt_init(&g_cert);
    rc = mbedtls_x509_crt_parse_der(&g_cert, bench_cert_der, bench_cert_der_len);
    if (rc) die("cert parse", rc);
    mbedtls_pk_init(&g_key);
    rc = mbedtls_pk_parse_key(&g_key, bench_key_der, bench_key_der_len, NULL, 0,
                              mbedtls_ctr_drbg_random, &g_drbg);
    if (rc) die("key parse", rc);

    mbedtls_ssl_ticket_init(&g_ticket);
    rc = mbedtls_ssl_ticket_setup(&g_ticket, mbedtls_ctr_drbg_random, &g_drbg,
                                  MBEDTLS_CIPHER_AES_128_GCM, 86400);
    if (rc) die("ticket setup", rc);

    for (unsigned round = 1;; round++) {
        printf("\n---------------- round %u ----------------\n", round);
        bench_aead();
        bench_suite("TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256",
                    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256);
        bench_suite("TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256",
                    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256);
        printf("\n(round %u done; repeats every 10 s)\n", round);
        sleep_ms(10000);
    }
}
