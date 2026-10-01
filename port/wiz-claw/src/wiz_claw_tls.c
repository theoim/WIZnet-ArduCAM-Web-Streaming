/**
 * wiz_claw_tls.c — mbedTLS 클라이언트 구현
 *
 * 엔트로피: RP2350 하드웨어 TRNG (get_rand_32, pico/rand.h)
 * BIO     : WIZnet ioLibrary send()/recv() 직접 연결
 * SNI     : mbedtls_ssl_set_hostname()
 * 서버 검증: WIZ_CLAW_CA_BUNDLE_PEM이 설정된 경우 VERIFY_REQUIRED,
 *            없으면 VERIFY_NONE + 경고 로그
 */
#include "wiz_claw_tls.h"
#include "wiz_claw_http.h"    /* wiz_claw_tcp_connect / wiz_claw_tcp_close */
#include "wiz_claw_ca_bundle.h"

#include "socket.h"           /* send, recv, getsockopt, getSn_SR */
#include "pico/rand.h"        /* get_rand_32() — RP2350 하드웨어 TRNG */
#include "pico/stdlib.h"      /* sleep_ms */
#include "pico/time.h"        /* to_ms_since_boot, get_absolute_time */

#include "mbedtls/error.h"

#include <string.h>

/* mbedtls/net_sockets.h는 베어메탈 환경에 존재하지 않으므로
 * BIO 콜백용 에러 코드를 SSL 레이어 코드로 매핑합니다. */
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED  MBEDTLS_ERR_SSL_INTERNAL_ERROR
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED  MBEDTLS_ERR_SSL_INTERNAL_ERROR
#endif
#ifndef MBEDTLS_ERR_NET_CONN_RESET
#define MBEDTLS_ERR_NET_CONN_RESET   MBEDTLS_ERR_SSL_CONN_EOF
#endif

static const char *TAG = "wiz_claw_tls";

/* ── RP2350 TRNG 엔트로피 콜백 ──────────────────────────────────
 *
 * mbedtls_entropy_add_source()에 등록.
 * get_rand_32()는 RP2350 TRNG 레지스터에서 32-bit 하드웨어 난수를 반환.
 */
static int trng_entropy_poll(void           *data,
                              unsigned char  *output,
                              size_t          len,
                              size_t         *olen)
{
    (void)data;
    size_t i = 0;

    /* 4바이트 단위로 채우기 */
    for (; i + sizeof(uint32_t) <= len; i += sizeof(uint32_t)) {
        uint32_t r = get_rand_32();
        memcpy(output + i, &r, sizeof(uint32_t));
    }
    /* 나머지 바이트 */
    if (i < len) {
        uint32_t r = get_rand_32();
        memcpy(output + i, &r, len - i);
    }
    *olen = len;
    return 0;
}

/* ── WIZnet 소켓 BIO 콜백 ───────────────────────────────────────
 *
 * mbedtls_ssl_set_bio()의 ctx로 소켓 번호(uint8_t)를 uintptr_t로 전달.
 */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    uint8_t  sn  = (uint8_t)(uintptr_t)ctx;
    uint16_t cap = (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len;
    int32_t  ret = send(sn, (uint8_t *)buf, cap);
    if (ret < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    /* W6300 TX 버퍼가 꽉 차면(원격이 약신호 등으로 ACK를 안 해 TX가 안 비면)
     * send()가 0을 반환한다. 이때 0을 그대로 돌려주면 mbedtls는 "0바이트 전송"
     * 으로 보고 같은 데이터를 계속 재전송해 내부에서 무한 루프에 빠진다 →
     * mbedtls_ssl_handshake()가 반환하지 않아 상위 handshake 루프의 데드라인
     * 체크에도 도달 못 하고 Core0 전체가 정지(2026-07-22 openai TLS handshake
     * 무한 hang 관측). WANT_WRITE를 반환해 mbedtls가 물러나게 하면, handshake
     * 루프가 WANT_WRITE를 보고 데드라인을 체크해 정상적으로 타임아웃한다. */
    if (ret == 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return (int)ret;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    uint8_t  sn  = (uint8_t)(uintptr_t)ctx;
    uint16_t avail = 0;

    getsockopt(sn, SO_RECVBUF, &avail);
    if (avail == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    uint16_t want = (avail < (uint16_t)len) ? avail : (uint16_t)len;
    int32_t  ret  = recv(sn, (uint8_t *)buf, want);
    if (ret < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)ret;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                             uint32_t timeout_ms)
{
    uint8_t  sn    = (uint8_t)(uintptr_t)ctx;
    uint32_t start = (uint32_t)to_ms_since_boot(get_absolute_time());
    uint16_t avail = 0;

    do {
        getsockopt(sn, SO_RECVBUF, &avail);
        if (avail > 0) {
            uint16_t want = (avail < (uint16_t)len) ? avail : (uint16_t)len;
            int32_t  ret  = recv(sn, (uint8_t *)buf, want);
            return (ret < 0) ? MBEDTLS_ERR_NET_RECV_FAILED : (int)ret;
        }
        /* 원격이 연결을 닫았으면 즉시 반환 */
        uint8_t sr = getSn_SR(sn);
        if ((sr == SOCK_CLOSED || sr == SOCK_CLOSE_WAIT) && avail == 0) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        sleep_ms(5);
    } while ((uint32_t)to_ms_since_boot(get_absolute_time()) - start < timeout_ms);

    return MBEDTLS_ERR_SSL_TIMEOUT;
}

/* ── wiz_claw_tls_init ──────────────────────────────────────────── */

wiz_claw_err_t wiz_claw_tls_init(wiz_claw_tls_t *tls,
                                   uint8_t         socket_no,
                                   uint32_t        timeout_ms)
{
    int ret;

    if (!tls) { return WIZ_CLAW_ERR_INVALID_ARG; }
    if (timeout_ms == 0) { timeout_ms = 30000u; }

    memset(tls, 0, sizeof(*tls));
    tls->socket_no = socket_no;

    mbedtls_entropy_init(&tls->entropy);
    mbedtls_ctr_drbg_init(&tls->ctr_drbg);
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_x509_crt_init(&tls->ca_cert);

    /* ① TRNG 엔트로피 소스 등록 */
    ret = mbedtls_entropy_add_source(&tls->entropy,
                                      trng_entropy_poll, NULL,
                                      16,                       /* min_threshold */
                                      MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (ret != 0) {
        WIZ_CLAW_LOG_E(TAG, "entropy_add_source -0x%04x", -ret);
        goto fail;
    }

    /* ② CTR-DRBG 시드 (RP2350 TRNG에서 엔트로피 수집) */
    ret = mbedtls_ctr_drbg_seed(&tls->ctr_drbg,
                                  mbedtls_entropy_func, &tls->entropy,
                                  (const unsigned char *)"wiz_claw_tls", 12);
    if (ret != 0) {
        WIZ_CLAW_LOG_E(TAG, "ctr_drbg_seed -0x%04x", -ret);
        goto fail;
    }

    /* ③ TLS 1.2 클라이언트 기본 설정 */
    ret = mbedtls_ssl_config_defaults(&tls->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        WIZ_CLAW_LOG_E(TAG, "ssl_config_defaults -0x%04x", -ret);
        goto fail;
    }

    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&tls->conf, timeout_ms);

    /* ④ CA 번들 로드 및 서버 검증 모드 설정 */
#if defined(WIZ_CLAW_CA_BUNDLE_PEM)
    {
        const char *pem = WIZ_CLAW_CA_BUNDLE_PEM;
        ret = mbedtls_x509_crt_parse(&tls->ca_cert,
                                      (const unsigned char *)pem,
                                      strlen(pem) + 1);   /* +1: NUL 포함 */
        if (ret < 0) {
            WIZ_CLAW_LOG_W(TAG, "CA 번들 파싱 실패 -0x%04x → VERIFY_NONE 폴백", -ret);
            mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
        } else {
            if (ret > 0) {
                WIZ_CLAW_LOG_W(TAG, "CA 번들 %d개 인증서 파싱 실패 (나머지는 로드됨)", ret);
            }
            mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->ca_cert, NULL);
            mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            WIZ_CLAW_LOG_I(TAG, "서버 인증서 검증 활성화");
        }
    }
#else
    WIZ_CLAW_LOG_W(TAG, "CA 번들 없음 — 서버 미검증 (개발 모드). "
                        "port/wiz-claw/tools/gen_ca_bundle.py 실행 후 wiz_claw_ca_bundle.h를 채우세요.");
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
#endif

    /* ⑤ SSL 컨텍스트에 설정 적용 + BIO 등록 */
    ret = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
    if (ret != 0) {
        WIZ_CLAW_LOG_E(TAG, "ssl_setup -0x%04x", -ret);
        goto fail;
    }

    mbedtls_ssl_set_bio(&tls->ssl,
                         (void *)(uintptr_t)socket_no,
                         bio_send, bio_recv, bio_recv_timeout);

    WIZ_CLAW_LOG_I(TAG, "TLS init ok sn=%u", socket_no);
    return WIZ_CLAW_OK;

fail:
    wiz_claw_tls_free(tls);
    return WIZ_CLAW_ERR_FAIL;
}

/* ── wiz_claw_tls_connect ──────────────────────────────────────── */

wiz_claw_err_t wiz_claw_tls_connect(wiz_claw_tls_t  *tls,
                                      const uint8_t    ip[4],
                                      uint16_t         port,
                                      const char      *hostname,
                                      uint32_t         timeout_ms)
{
    int            ret;
    wiz_claw_err_t err;

    if (!tls || !ip) { return WIZ_CLAW_ERR_INVALID_ARG; }

    /* SNI: 서버가 여러 인증서를 호스팅할 때 올바른 인증서 선택 */
    if (hostname) {
        ret = mbedtls_ssl_set_hostname(&tls->ssl, hostname);
        if (ret != 0) {
            WIZ_CLAW_LOG_E(TAG, "ssl_set_hostname -0x%04x", -ret);
            return WIZ_CLAW_ERR_FAIL;
        }
    }

    /* TCP 연결 (WIZnet 하드웨어 TCP) */
    err = wiz_claw_tcp_connect(tls->socket_no, ip, port, timeout_ms);
    if (err != WIZ_CLAW_OK) { return err; }

    /* TLS 핸드셰이크 */
    WIZ_CLAW_LOG_I(TAG, "TLS handshake %s:%u ...",
                   hostname ? hostname : "?", port);

    {
        uint32_t hs_start = (uint32_t)to_ms_since_boot(get_absolute_time());
        uint32_t hs_limit = (timeout_ms > 0) ? timeout_ms : 30000u;
        do {
            ret = mbedtls_ssl_handshake(&tls->ssl);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if ((uint32_t)to_ms_since_boot(get_absolute_time()) - hs_start >= hs_limit) {
                    WIZ_CLAW_LOG_E(TAG, "TLS handshake timeout after %ums", (unsigned)hs_limit);
                    ret = MBEDTLS_ERR_SSL_TIMEOUT;
                    break;
                }
                sleep_ms(5);
            }
        } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                 ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    }

    if (ret != 0) {
        char errbuf[80];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        WIZ_CLAW_LOG_E(TAG, "TLS handshake failed -0x%04x: %s", -ret, errbuf);
        /* X509 검증 실패 플래그: 0x08=NOT_TRUSTED, 0x4000=BAD_KEY */
        uint32_t vf = mbedtls_ssl_get_verify_result(&tls->ssl);
        WIZ_CLAW_LOG_E(TAG, "X509 verify_result=0x%08x", (unsigned)vf);
        wiz_claw_tcp_close(tls->socket_no);
        return WIZ_CLAW_ERR_HTTP;
    }

    {
        uint32_t vf = mbedtls_ssl_get_verify_result(&tls->ssl);
        WIZ_CLAW_LOG_I(TAG, "TLS ok [%s] verify_flags=0x%08x",
                        mbedtls_ssl_get_ciphersuite(&tls->ssl), (unsigned)vf);
    }
    return WIZ_CLAW_OK;
}

/* ── wiz_claw_tls_write ─────────────────────────────────────────── */

int32_t wiz_claw_tls_write(wiz_claw_tls_t *tls,
                             const uint8_t  *buf,
                             size_t          len)
{
    size_t   written  = 0;
    int      ret;
    uint32_t deadline = (uint32_t)to_ms_since_boot(get_absolute_time()) + 30000u;

    while (written < len) {
        ret = mbedtls_ssl_write(&tls->ssl, buf + written, len - written);
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if ((uint32_t)to_ms_since_boot(get_absolute_time()) >= deadline) {
                WIZ_CLAW_LOG_E(TAG, "TLS write timeout");
                return MBEDTLS_ERR_SSL_TIMEOUT;
            }
            sleep_ms(5);
            continue;
        }
        if (ret < 0) { return (int32_t)ret; }
        written += (size_t)ret;
    }
    return (int32_t)written;
}

/* ── wiz_claw_tls_read ──────────────────────────────────────────── */

int32_t wiz_claw_tls_read(wiz_claw_tls_t *tls,
                            uint8_t        *buf,
                            size_t          len,
                            uint32_t        timeout_ms)
{
    /* timeout_ms는 init 시점에 ssl_conf_read_timeout으로 설정됨.
     * 추가적인 per-call timeout 필요 시 mbedtls_ssl_conf_read_timeout 재설정. */
    (void)timeout_ms;

    int ret = mbedtls_ssl_read(&tls->ssl, buf, len);

    if (ret == MBEDTLS_ERR_SSL_WANT_READ)         { return 0; }
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)  { return 0; }
    if (ret == 0)                                   { return 0; }
    if (ret < 0)                                    { return -1; }
    return (int32_t)ret;
}

/* ── wiz_claw_tls_close ─────────────────────────────────────────── */

void wiz_claw_tls_close(wiz_claw_tls_t *tls)
{
    int ret;
    if (!tls) { return; }

    /* close_notify 교환 (정중한 TLS 종료) */
    do {
        ret = mbedtls_ssl_close_notify(&tls->ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    wiz_claw_tcp_close(tls->socket_no);
}

/* ── wiz_claw_tls_free ──────────────────────────────────────────── */

void wiz_claw_tls_free(wiz_claw_tls_t *tls)
{
    if (!tls) { return; }
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_ctr_drbg_free(&tls->ctr_drbg);
    mbedtls_entropy_free(&tls->entropy);
    mbedtls_x509_crt_free(&tls->ca_cert);
}
