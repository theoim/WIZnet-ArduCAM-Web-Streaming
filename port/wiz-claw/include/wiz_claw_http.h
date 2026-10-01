/**
 * wiz_claw_http.h — HTTP/1.1 클라이언트 (WIZnet 소켓 위 순수 TCP)
 *
 * TLS 없음 — 나중에 mbedTLS 레이어를 얹을 수 있도록
 * TCP transport 부분을 함수 포인터로 분리해 놓음.
 *
 * 제공하는 것:
 *   - wiz_claw_http_post_cb  → wiz_claw_llm_config_t.http_post 에 대입
 *   - wiz_claw_http_call_cb  → wiz_claw_tg_config_t.http_call  에 대입
 *
 * 의존: ioLibrary_Driver (socket.h), wiz_claw_net (DNS),
 *       Pico SDK (pico/stdlib.h)
 */
#pragma once

#include "wiz_claw.h"
/*
 * The upstream header pulls in wiz_claw_telegram.h and wiz_claw_llm.h here, for
 * two callback typedefs that only appear in comments. llm.h in turn includes
 * cJSON, which would bring 3,470 lines of JSON parser into a firmware whose
 * entire JSON output is {"content":"..."}.
 *
 * Cut, since nothing declared below refers to either type.
 */

/* ── 소켓 번호 기본값 ─────────────────────────────────────────
 *
 *  0 : Telegram getUpdates  (긴 연결)
 *  1 : Telegram sendMessage (짧은 요청)
 *  2 : LLM API              (긴 응답)
 *  3 : 기타 HTTP
 *  4 : 예비
 *  5 : DNS  (UDP)           ← wiz_claw_net.h 참조
 *  6 : DHCP (UDP)           ← wiz_claw_net.h 참조
 *  7 : 예비
 */
#ifndef WIZ_CLAW_SOCK_TG_POLL
#define WIZ_CLAW_SOCK_TG_POLL   0
#endif
#ifndef WIZ_CLAW_SOCK_TG_SEND
#define WIZ_CLAW_SOCK_TG_SEND   1
#endif
#ifndef WIZ_CLAW_SOCK_LLM
#define WIZ_CLAW_SOCK_LLM       2
#endif

/* ── 수신 버퍼 크기 ─────────────────────────────────────────── */
#ifndef WIZ_CLAW_HTTP_HEADER_BUF   /* 헤더 최대 크기 */
#define WIZ_CLAW_HTTP_HEADER_BUF  2048
#endif
#ifndef WIZ_CLAW_HTTP_BODY_INIT    /* 바디 초기 heap 크기 */
#define WIZ_CLAW_HTTP_BODY_INIT   4096
#endif
#ifndef WIZ_CLAW_HTTP_BODY_MAX     /* 바디 최대 허용 크기 */
#define WIZ_CLAW_HTTP_BODY_MAX    65536
#endif

/* ── HTTP 컨텍스트 ────────────────────────────────────────────
 *
 * wiz_claw_http_post_fn / wiz_claw_http_call_fn 의 user_ctx로 전달.
 * 스택 또는 전역 변수로 선언해 사용.
 */
typedef struct {
    uint8_t  socket_no;      /* 이 요청에 사용할 WIZnet 소켓 번호 */
    uint32_t timeout_ms;     /* 연결/수신 타임아웃. 0 → 기본값 30000 */
} wiz_claw_http_ctx_t;

/* ── 파싱된 URL ───────────────────────────────────────────────── */
typedef struct {
    char     host[128];
    char     path[512];
    uint16_t port;
    bool     is_https;
} wiz_claw_parsed_url_t;

/* ── URL 파서 (내부에서도 쓰지만 외부 노출) ──────────────────── */
wiz_claw_err_t wiz_claw_http_parse_url(const char              *url,
                                        wiz_claw_parsed_url_t   *out);

/* ── 저수준 TCP 헬퍼 ──────────────────────────────────────────── */

/** TCP 소켓 열고 IP:port 연결 (블로킹). */
wiz_claw_err_t wiz_claw_tcp_connect(uint8_t sn,
                                     const uint8_t ip[4],
                                     uint16_t port,
                                     uint32_t timeout_ms);

/** TCP 소켓 닫기. */
void wiz_claw_tcp_close(uint8_t sn);

/** 데이터 전송 (모두 보낼 때까지 반복). */
wiz_claw_err_t wiz_claw_tcp_send_all(uint8_t        sn,
                                      const uint8_t *data,
                                      size_t         len,
                                      uint32_t       timeout_ms);

/** 수신 (최소 min_len 바이트가 들어올 때까지 기다림). */
int32_t wiz_claw_tcp_recv_wait(uint8_t   sn,
                                uint8_t  *buf,
                                uint16_t  len,
                                uint32_t  timeout_ms);

/* ── HTTP 요청 / 응답 ─────────────────────────────────────────── */

/**
 * HTTP POST 요청 실행 (DNS 해석 → TCP 연결 → 전송 → 수신 → 소켓 닫기).
 *
 * wiz_claw_http_post_fn 시그니처와 동일 → LLM 콜백으로 직접 대입 가능.
 *
 *   config.http_post     = wiz_claw_http_post_cb;
 *   config.http_user_ctx = &my_http_ctx;
 */
wiz_claw_err_t wiz_claw_http_post_cb(const char *url,
                                      const char *auth_header,
                                      const char *body_json,
                                      char      **out_response,
                                      void       *user_ctx);

/**
 * POST with an explicit content type and an explicit length.
 *
 * For bodies that are neither JSON nor text - a multipart envelope carrying a
 * JPEG, where strlen() would stop at the first 0x00 inside the image and send
 * a truncated file.
 */
wiz_claw_err_t wiz_claw_http_post_raw(const char *url,
                                       const char *content_type,
                                       const void *body,
                                       size_t      body_len,
                                       char      **out_response,
                                       void       *user_ctx);

/* 마지막 HTTP 응답 상태 코드 반환 (wiz_claw_http_post_cb 호출 직후 유효) */
int wiz_claw_http_last_status(void);

/**
 * HTTP GET/POST 요청 실행 (auth 헤더 없음).
 *
 * wiz_claw_http_call_fn 시그니처와 동일 → Telegram 콜백으로 직접 대입 가능.
 *
 *   tg_config.http_call     = wiz_claw_http_call_cb;
 *   tg_config.http_user_ctx = &my_http_ctx;
 */
wiz_claw_err_t wiz_claw_http_call_cb(const char *url,
                                      const char *body_json,
                                      char      **out_response,
                                      void       *user_ctx);
