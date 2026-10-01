/*
 * wiz_claw.h — 공통 타입, 에러 코드, 로그 매크로
 *
 * ESP-IDF / Pico SDK 의존성 없음. 표준 C만 사용.
 * 로그 매크로는 빌드 환경에 맞게 재정의 가능.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── 에러 코드 ─────────────────────────────────────────────── */
typedef enum {
    WIZ_CLAW_OK              =  0,
    WIZ_CLAW_ERR_INVALID_ARG = -1,
    WIZ_CLAW_ERR_NO_MEM      = -2,
    WIZ_CLAW_ERR_NOT_FOUND   = -3,
    WIZ_CLAW_ERR_HTTP        = -4,
    WIZ_CLAW_ERR_JSON        = -5,
    WIZ_CLAW_ERR_FAIL        = -6,
} wiz_claw_err_t;

/* ── 로그 매크로 (재정의 가능) ──────────────────────────────── */
#ifndef WIZ_CLAW_LOG_I
#define WIZ_CLAW_LOG_I(tag, fmt, ...) \
    printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

#ifndef WIZ_CLAW_LOG_W
#define WIZ_CLAW_LOG_W(tag, fmt, ...) \
    printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

#ifndef WIZ_CLAW_LOG_E
#define WIZ_CLAW_LOG_E(tag, fmt, ...) \
    printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

/* ── 내부 유틸: heap 문자열 포맷 ────────────────────────────── */
static inline char *wiz_claw_dup_printf(const char *fmt, ...)
{
    va_list args, copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) { va_end(args); return NULL; }

    buf = (char *)calloc(1, (size_t)needed + 1);
    if (!buf) { va_end(args); return NULL; }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}
