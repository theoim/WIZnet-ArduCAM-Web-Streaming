/**
 * wiz_claw_tls.h — mbedTLS 클라이언트 (WIZnet 소켓 BIO 연동)
 *
 * 엔트로피: RP2350 하드웨어 TRNG (pico/rand.h get_rand_32())
 * 서버 검증: wiz_claw_ca_bundle.h의 WIZ_CLAW_CA_BUNDLE_PEM이 설정된 경우 활성화
 *
 * 사용 흐름:
 *   wiz_claw_tls_init()    → CTR-DRBG 초기화, CA 로드
 *   wiz_claw_tls_connect() → TCP 연결 + TLS 핸드셰이크
 *   wiz_claw_tls_write()   → mbedtls_ssl_write 래퍼
 *   wiz_claw_tls_read()    → mbedtls_ssl_read 래퍼
 *   wiz_claw_tls_close()   → close_notify + TCP 닫기
 *   wiz_claw_tls_free()    → mbedTLS 컨텍스트 해제
 *
 * 의존: mbedTLS, ioLibrary (socket.h), Pico SDK (pico/rand.h, pico/time.h)
 */
#pragma once

#include "wiz_claw.h"

#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* ── TLS 세션 컨텍스트 ────────────────────────────────────────────
 *
 * 크기가 크므로 (약 1-2 KB) 스택에 두지 말고 전역 또는 정적 변수로 선언.
 * HTTP 모듈 내부에서 static으로 관리하므로 외부에서 직접 할당 불필요.
 */
typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca_cert;
    uint8_t                  socket_no;
} wiz_claw_tls_t;

/* ── API ──────────────────────────────────────────────────────────── */

/**
 * TLS 컨텍스트 초기화.
 *   - RP2350 TRNG 엔트로피 소스 등록
 *   - CTR-DRBG 시드
 *   - CA 번들 로드 (WIZ_CLAW_CA_BUNDLE_PEM이 정의된 경우)
 *   - mbedtls_ssl_setup + BIO 콜백 등록
 *
 * @param tls        초기화할 컨텍스트 (호출자가 memset 0 불필요)
 * @param socket_no  이 TLS 세션에 사용할 WIZnet 소켓 번호
 * @param timeout_ms 핸드셰이크 / 수신 타임아웃. 0 → 기본값 30000ms
 */
wiz_claw_err_t wiz_claw_tls_init(wiz_claw_tls_t *tls,
                                   uint8_t         socket_no,
                                   uint32_t        timeout_ms);

/**
 * TCP 연결 후 TLS 핸드셰이크 수행 (블로킹).
 *
 * @param ip        서버 IPv4 주소 (DNS 해석 후 전달)
 * @param port      포트 번호 (HTTPS = 443)
 * @param hostname  SNI 호스트명. NULL이면 SNI 생략 (권장하지 않음)
 * @param timeout_ms 연결/핸드셰이크 타임아웃
 */
wiz_claw_err_t wiz_claw_tls_connect(wiz_claw_tls_t  *tls,
                                      const uint8_t    ip[4],
                                      uint16_t         port,
                                      const char      *hostname,
                                      uint32_t         timeout_ms);

/**
 * TLS를 통해 데이터 전송. 내부적으로 모두 전송될 때까지 반복.
 * @return 전송된 바이트 수 (음수 = mbedTLS 오류 코드)
 */
int32_t wiz_claw_tls_write(wiz_claw_tls_t *tls,
                             const uint8_t  *buf,
                             size_t          len);

/**
 * TLS를 통해 데이터 수신.
 * @return 수신 바이트 수, 0 = EOF/close, 음수 = 오류
 */
int32_t wiz_claw_tls_read(wiz_claw_tls_t *tls,
                            uint8_t        *buf,
                            size_t          len,
                            uint32_t        timeout_ms);

/** TLS close_notify 전송 후 TCP 소켓 닫기. */
void wiz_claw_tls_close(wiz_claw_tls_t *tls);

/** mbedTLS 컨텍스트 메모리 해제 (wiz_claw_tls_close 이후 호출). */
void wiz_claw_tls_free(wiz_claw_tls_t *tls);
