/**
 * @file    http_req_test.c
 * @brief   Host tests for http/http_req.c. No board, no SDK.
 *
 * Build and run: test/run_host_tests.sh (MinGW gcc). Every case feeds the
 * request whole, byte by byte, two bytes at a time, at every single split
 * point, and in random splits, and expects the same outcome each way - the
 * parser must not care how the TLS layer slices a request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_req.h"

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    else g_pass++; } while (0)

/* ------------------------------------------------------------ feed drivers */

/* Feed all of req in chunks given by a split plan; return the final result and
   the total consumed. A plan is a list of chunk sizes; 0 ends the plan and the
   remainder is fed as one chunk. */
static http_req_result_t feed_plan(http_req_t *r, const uint8_t *req, size_t len,
                                   const size_t *plan, size_t *total_consumed)
{
    http_req_init(r);
    size_t pos = 0, pi = 0, consumed_total = 0;
    http_req_result_t res = HTTP_REQ_NEED_MORE;
    while (pos < len) {
        size_t n = plan && plan[pi] ? plan[pi] : len - pos;
        if (n > len - pos) n = len - pos;
        size_t c = 0;
        res = http_req_feed(r, req + pos, n, &c);
        consumed_total += c;
        if (res != HTTP_REQ_NEED_MORE) {
            /* after a terminal result, leftover bytes must not be consumed */
            if (c < n) {
                size_t c2 = 0;
                http_req_result_t r2 = http_req_feed(r, req + pos + c, n - c, &c2);
                CHECK(r2 == HTTP_REQ_ERROR && c2 == 0, "post-terminal feed must consume nothing");
                CHECK(http_req_error(r) == (res == HTTP_REQ_COMPLETE ? HTTP_ERR_STATE : http_req_error(r)),
                      "post-terminal error code");
            }
            break;
        }
        pos += n;
    }
    if (total_consumed) *total_consumed = consumed_total;
    return res;
}

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

/* Run req through whole / 1-byte / 2-byte / every split / 64 random plans.
   Expect the same result and (on COMPLETE) the same path, query, body. */
static void expect_all_splits(const char *name, const uint8_t *req, size_t len,
                              http_req_result_t want, http_req_err_t want_err)
{
    /* static: these are large (two contexts and a 1600-entry plan) and the
       compiler inlines this function many times into main, which overflowed
       the 1 MB Windows stack with them on it */
    static http_req_t ref;
    size_t ref_consumed = 0;
    http_req_result_t res = feed_plan(&ref, req, len, NULL, &ref_consumed);
    CHECK(res == want, "%s: whole feed result %d want %d (%s)", name, res, want,
          http_req_err_name(http_req_error(&ref)));
    if (want == HTTP_REQ_ERROR) {
        CHECK(http_req_error(&ref) == want_err, "%s: err %s want %s", name,
              http_req_err_name(http_req_error(&ref)), http_req_err_name(want_err));
    }

    static http_req_t r;
    static size_t plan[1600];

    /* 1-byte and 2-byte */
    for (size_t step = 1; step <= 2; step++) {
        size_t k = 0;
        for (size_t p = 0; p < len && k < 1599; p += step) plan[k++] = step;
        plan[k] = 0;
        res = feed_plan(&r, req, len, plan, NULL);
        CHECK(res == want && http_req_error(&r) == http_req_error(&ref),
              "%s: %zu-byte feed result %d/%s", name, step, res, http_req_err_name(http_req_error(&r)));
        if (want == HTTP_REQ_COMPLETE) {
            CHECK(r.path_len == ref.path_len && memcmp(r.path, ref.path, r.path_len) == 0 &&
                  r.body_len == ref.body_len && memcmp(r.body, ref.body, r.body_len) == 0 &&
                  r.query.len == ref.query.len, "%s: %zu-byte feed content differs", name, step);
        }
    }
    /* every single split point */
    for (size_t s = 1; s < len; s++) {
        plan[0] = s; plan[1] = 0;
        res = feed_plan(&r, req, len, plan, NULL);
        if (!(res == want && http_req_error(&r) == http_req_error(&ref))) {
            CHECK(0, "%s: split at %zu gives %d/%s", name, s, res, http_req_err_name(http_req_error(&r)));
            break;
        }
    }
    /* random plans */
    for (int t = 0; t < 64; t++) {
        size_t k = 0, pos = 0;
        while (pos < len && k < 1599) {
            size_t n = 1 + (rnd() % 37);
            plan[k++] = n; pos += n;
        }
        plan[k] = 0;
        res = feed_plan(&r, req, len, plan, NULL);
        if (!(res == want && http_req_error(&r) == http_req_error(&ref))) {
            CHECK(0, "%s: random plan %d gives %d/%s", name, t, res, http_req_err_name(http_req_error(&r)));
            break;
        }
    }
}

#define S(lit) ((const uint8_t *)(lit)), (sizeof(lit) - 1)

static void ok(const char *name, const char *req)
{
    expect_all_splits(name, (const uint8_t *)req, strlen(req), HTTP_REQ_COMPLETE, HTTP_ERR_NONE);
}
static void bad(const char *name, const char *req, http_req_err_t e)
{
    expect_all_splits(name, (const uint8_t *)req, strlen(req), HTTP_REQ_ERROR, e);
}

/* ------------------------------------------------------------------ cases */

static __attribute__((noinline)) void test_basic_get(void)
{
    http_req_t r; size_t n;
    const char *req = "GET /api/status HTTP/1.1\r\nHost: 192.168.11.7\r\nAccept: */*\r\n\r\n";
    CHECK(feed_plan(&r, (const uint8_t *)req, strlen(req), NULL, &n) == HTTP_REQ_COMPLETE, "basic GET");
    CHECK(n == strlen(req), "basic GET consumed %zu", n);
    CHECK(http_req_method(&r) == HTTP_METHOD_GET, "method");
    size_t l; const char *p = http_req_path(&r, &l);
    CHECK(l == 11 && strcmp(p, "/api/status") == 0, "path '%s'", p);
    CHECK(http_req_query(&r, &l) && l == 0, "no query");
    const uint8_t *h = http_req_header(&r, HTTP_H_HOST, &l);
    CHECK(h && l == 12 && memcmp(h, "192.168.11.7", 12) == 0, "host");
    CHECK(http_req_header(&r, HTTP_H_COOKIE, &l) == NULL, "no cookie");
    CHECK(http_req_body(&r, &l) && l == 0, "no body");
    CHECK(http_req_status(&r) == 0, "status 0");
    ok("basic GET splits", req);
}

static __attribute__((noinline)) void test_existing_routes(void)
{
    /* every route and query shape the current page sends */
    ok("stream", "GET /stream?1761200000000&r=idle&p=1,11900 HTTP/1.1\r\nHost: cam\r\n\r\n");
    ok("res",    "GET /api/res?v=800x600 HTTP/1.1\r\nHost: cam\r\n\r\n");
    ok("clk",    "GET /api/clk?div=1&pll=4 HTTP/1.1\r\nHost: cam\r\n\r\n");
    ok("load",   "GET /load?kb=64&n=3 HTTP/1.1\r\nHost: cam\r\n\r\n");
    ok("cam",    "GET /api/cam?brightness=3 HTTP/1.1\r\nHost: cam\r\n\r\n");
    ok("logo",   "GET /logo.png HTTP/1.1\r\nHost: cam\r\n\r\n");
    ok("root",   "GET / HTTP/1.1\r\nHost: cam\r\n\r\n");
    ok("http10", "GET /api/status HTTP/1.0\r\n\r\n");            /* no Host needed */

    http_req_t r; const char *req = "GET /stream?1761200000000&r=idle&p=1,11900 HTTP/1.1\r\nHost: cam\r\n\r\n";
    feed_plan(&r, (const uint8_t *)req, strlen(req), NULL, NULL);
    size_t ql; const uint8_t *q = http_req_query(&r, &ql);
    char v[32];
    CHECK(http_query_get(q, ql, "r", v, sizeof v) == 4 && strcmp(v, "idle") == 0, "query r=%s", v);
    CHECK(http_query_get(q, ql, "p", v, sizeof v) == 7 && strcmp(v, "1,11900") == 0, "query p=%s", v);
    CHECK(http_query_get(q, ql, "1761200000000", v, sizeof v) == 0, "bare key");
    CHECK(http_query_get(q, ql, "zz", v, sizeof v) == -1, "absent key");
    CHECK(http_query_get(S("a=1%20b+c&d=%zz"), "a", v, sizeof v) == 5 && strcmp(v, "1 b c") == 0, "decode %%20 and +");
    CHECK(http_query_get(S("a=1&d=%zz"), "d", v, sizeof v) == -2, "bad escape in value");
    CHECK(http_query_get(S("a=0123456789"), "a", v, 4) == -2, "value does not fit");
}

static __attribute__((noinline)) void test_browser_like(void)
{
    /* Chrome-ish request with a cookie and many headers, ~700 B */
    const char *req =
        "GET /api/status HTTP/1.1\r\n"
        "Host: 192.168.11.7\r\n"
        "Connection: keep-alive\r\n"
        "sec-ch-ua: \"Chromium\";v=\"131\", \"Not_A Brand\";v=\"24\"\r\n"
        "sec-ch-ua-mobile: ?0\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36\r\n"
        "Accept: application/json, text/plain, */*\r\n"
        "Sec-Fetch-Site: same-origin\r\n"
        "Sec-Fetch-Mode: cors\r\n"
        "Sec-Fetch-Dest: empty\r\n"
        "Referer: https://192.168.11.7/\r\n"
        "Accept-Encoding: gzip, deflate, br, zstd\r\n"
        "Accept-Language: ko-KR,ko;q=0.9,en-US;q=0.8,en;q=0.7\r\n"
        "Cookie: cam_session=0123456789abcdef0123456789abcdef; theme=dark\r\n"
        "Authorization: Bearer abc.def.ghi\r\n"
        "\r\n";
    ok("browser GET", req);
    http_req_t r; feed_plan(&r, (const uint8_t *)req, strlen(req), NULL, NULL);
    size_t l; const uint8_t *c = http_req_header(&r, HTTP_H_COOKIE, &l);
    static const char want_cookie[] = "cam_session=0123456789abcdef0123456789abcdef; theme=dark";
    CHECK(c && l == sizeof want_cookie - 1 && memcmp(c, want_cookie, l) == 0, "cookie span (%zu)", l);
    const uint8_t *a = http_req_header(&r, HTTP_H_AUTHORIZATION, &l);
    CHECK(a && l == 18 && memcmp(a, "Bearer abc.def.ghi", 18) == 0, "authorization span");
    CHECK(r.keep_alive_requested, "keep-alive noted (server still closes)");
}

static __attribute__((noinline)) void test_post(void)
{
    const char *req =
        "POST /api/login HTTP/1.1\r\nHost: cam\r\nContent-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 27\r\n\r\nuser=admin&pass=p%40ss+word";
    ok("POST with body", req);
    http_req_t r; feed_plan(&r, (const uint8_t *)req, strlen(req), NULL, NULL);
    size_t l; const uint8_t *b = http_req_body(&r, &l);
    CHECK(l == 27 && memcmp(b, "user=admin&pass=p%40ss+word", 27) == 0, "body bytes");
    char v[32];
    CHECK(http_query_get(b, l, "pass", v, sizeof v) == 9 && strcmp(v, "p@ss word") == 0, "form decode '%s'", v);
    CHECK(http_req_method(&r) == HTTP_METHOD_POST, "POST method");

    ok("empty POST", "POST /api/x HTTP/1.1\r\nHost: cam\r\nContent-Length: 0\r\n\r\n");
    bad("POST without CL", "POST /api/x HTTP/1.1\r\nHost: cam\r\n\r\n", HTTP_ERR_LENGTH_REQUIRED);

    /* body split exactly at header end, and header+body in one chunk, are
       both covered by expect_all_splits; here: body arrives in 3 chunks */
    size_t plan[] = { 60, 30, 10, 0 };
    CHECK(feed_plan(&r, (const uint8_t *)req, strlen(req), plan, &l) == HTTP_REQ_COMPLETE && l == strlen(req),
          "POST 3-chunk");

    /* body ends, then more bytes: not consumed */
    const char *two = "POST /a HTTP/1.1\r\nHost: c\r\nContent-Length: 3\r\n\r\nabcGET /b HTTP/1.1\r\n\r\n";
    size_t c = 0;
    http_req_init(&r);
    CHECK(http_req_feed(&r, (const uint8_t *)two, strlen(two), &c) == HTTP_REQ_COMPLETE, "two requests: first completes");
    CHECK(c == strlen(two) - strlen("GET /b HTTP/1.1\r\n\r\n"), "second request left unconsumed (%zu)", c);
    CHECK(r.body_len == 3 && memcmp(r.body, "abc", 3) == 0, "first body intact");

    /* incomplete body: stays NEED_MORE (the connection layer times it out) */
    const char *part = "POST /a HTTP/1.1\r\nHost: c\r\nContent-Length: 10\r\n\r\nabc";
    CHECK(feed_plan(&r, (const uint8_t *)part, strlen(part), NULL, NULL) == HTTP_REQ_NEED_MORE, "incomplete body");
    CHECK(http_req_headers_done(&r) && r.content_length == 10 && r.body_len == 3, "headers done, body partial");
}

static __attribute__((noinline)) void test_limits(void)
{
    static char buf[4096];
    /* header block exactly at the limit: accepted */
    {
        const char *head = "GET /x HTTP/1.1\r\nHost: c\r\nX-Pad: ";
        size_t pad = HTTP_REQ_HDR_MAX - strlen(head) - 4;     /* "\r\n\r\n" */
        memcpy(buf, head, strlen(head));
        memset(buf + strlen(head), 'a', pad);
        memcpy(buf + strlen(head) + pad, "\r\n\r\n", 4);
        expect_all_splits("header at limit", (const uint8_t *)buf, HTTP_REQ_HDR_MAX, HTTP_REQ_COMPLETE, HTTP_ERR_NONE);
    }
    /* one byte over: 431 */
    {
        const char *head = "GET /x HTTP/1.1\r\nHost: c\r\nX-Pad: ";
        size_t pad = HTTP_REQ_HDR_MAX - strlen(head) - 4 + 1;
        memcpy(buf, head, strlen(head));
        memset(buf + strlen(head), 'a', pad);
        memcpy(buf + strlen(head) + pad, "\r\n\r\n", 4);
        expect_all_splits("header over limit", (const uint8_t *)buf, HTTP_REQ_HDR_MAX + 1, HTTP_REQ_ERROR, HTTP_ERR_HEADER_TOO_LARGE);
    }
    /* body at and over the limit */
    {
        int n = snprintf(buf, sizeof buf, "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: %d\r\n\r\n", HTTP_REQ_BODY_MAX);
        memset(buf + n, 'b', HTTP_REQ_BODY_MAX);
        expect_all_splits("body at limit", (const uint8_t *)buf, (size_t)n + HTTP_REQ_BODY_MAX, HTTP_REQ_COMPLETE, HTTP_ERR_NONE);
        n = snprintf(buf, sizeof buf, "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: %d\r\n\r\n", HTTP_REQ_BODY_MAX + 1);
        expect_all_splits("body over limit", (const uint8_t *)buf, (size_t)n, HTTP_REQ_ERROR, HTTP_ERR_BODY_TOO_LARGE);
    }
    /* path at and over the limit */
    {
        int n = snprintf(buf, sizeof buf, "GET /");
        memset(buf + n, 'p', HTTP_REQ_PATH_MAX - 2); n += HTTP_REQ_PATH_MAX - 2;        /* total path = MAX-1 */
        n += snprintf(buf + n, sizeof buf - n, " HTTP/1.1\r\nHost: c\r\n\r\n");
        expect_all_splits("path at limit", (const uint8_t *)buf, (size_t)n, HTTP_REQ_COMPLETE, HTTP_ERR_NONE);
        n = snprintf(buf, sizeof buf, "GET /");
        memset(buf + n, 'p', HTTP_REQ_PATH_MAX - 1); n += HTTP_REQ_PATH_MAX - 1;        /* total path = MAX */
        n += snprintf(buf + n, sizeof buf - n, " HTTP/1.1\r\nHost: c\r\n\r\n");
        expect_all_splits("path over limit", (const uint8_t *)buf, (size_t)n, HTTP_REQ_ERROR, HTTP_ERR_TARGET_TOO_LONG);
    }
    /* query over limit */
    {
        int n = snprintf(buf, sizeof buf, "GET /x?");
        memset(buf + n, 'q', HTTP_REQ_QUERY_MAX + 1); n += HTTP_REQ_QUERY_MAX + 1;
        n += snprintf(buf + n, sizeof buf - n, " HTTP/1.1\r\nHost: c\r\n\r\n");
        expect_all_splits("query over limit", (const uint8_t *)buf, (size_t)n, HTTP_REQ_ERROR, HTTP_ERR_TARGET_TOO_LONG);
    }
}

static __attribute__((noinline)) void test_request_line(void)
{
    bad("empty method",      " /x HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE);
    bad("no target",         "GET  HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE);
    bad("no version",        "GET /x\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE);
    bad("double space",      "GET  /x HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE);
    bad("tab separator",     "GET\t/x HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE);
    bad("garbage version",   "GET /x HTTX/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE);
    bad("HTTP/2.0",          "GET /x HTTP/2.0\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_VERSION);
    bad("HTTP/0.9",          "GET /x HTTP/0.9\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_VERSION);
    bad("HTTP/1.2",          "GET /x HTTP/1.2\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_VERSION);
    bad("long method",       "ABCDEFGHIJKLMNOPQ /x HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_REQUEST_LINE);
    bad("absolute form",     "GET http://cam/x HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("asterisk form",     "OPTIONS * HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("bad pct",           "GET /a%zz HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("short pct",         "GET /a%4 HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("pct NUL",           "GET /a%00b HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("pct ctl",           "GET /a%0Ab HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("dotdot",            "GET /api/../x HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("dotdot encoded",    "GET /api/%2e%2e/x HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("dotdot at end",     "GET /api/.. HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("bad char",          "GET /a\"b HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("non-ascii",         "GET /a\xC3\xA9 HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);   /* raw bytes >= 0x80 are not pchar */
    bad("bad query char",    "GET /a?b=\"c HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    bad("bad query pct",     "GET /a?b=%G0 HTTP/1.1\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_TARGET);
    ok("dots ok",            "GET /a/./b.. HTTP/1.1\r\nHost: c\r\n\r\n");   /* ".." only as a whole segment */
    ok("encoded ok",         "GET /a%20b?x=%2F HTTP/1.1\r\nHost: c\r\n\r\n");
    ok("lowercase method",   "get /x HTTP/1.1\r\nHost: c\r\n\r\n");         /* token; classified GET */

    http_req_t r;
    const char *o = "DELETE /x HTTP/1.1\r\nHost: c\r\n\r\n";
    CHECK(feed_plan(&r, (const uint8_t *)o, strlen(o), NULL, NULL) == HTTP_REQ_COMPLETE, "other method parses");
    CHECK(http_req_method(&r) == HTTP_METHOD_OTHER, "other method classified for 405");
    size_t l; const uint8_t *m = http_req_method_token(&r, &l);
    CHECK(l == 6 && memcmp(m, "DELETE", 6) == 0, "method token");
    const char *e = "GET /a%20b?x=%2F HTTP/1.1\r\nHost: c\r\n\r\n";
    feed_plan(&r, (const uint8_t *)e, strlen(e), NULL, NULL);
    const char *p = http_req_path(&r, &l);
    CHECK(l == 4 && strcmp(p, "/a b") == 0, "path decoded '%s'", p);
    const uint8_t *q = http_req_query(&r, &l);
    CHECK(l == 5 && memcmp(q, "x=%2F", 5) == 0, "query kept raw");
}

static __attribute__((noinline)) void test_headers(void)
{
    bad("no Host 1.1",       "GET /x HTTP/1.1\r\nAccept: */*\r\n\r\n", HTTP_ERR_HOST_MISSING);
    bad("dup Host",          "GET /x HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", HTTP_ERR_HOST_DUPLICATE);
    bad("dup Host 1.0",      "GET /x HTTP/1.0\r\nHost: a\r\nhost: a\r\n\r\n", HTTP_ERR_HOST_DUPLICATE);
    ok("case-insensitive",   "GET /x HTTP/1.1\r\nhOsT: c\r\ncookie: a=1\r\n\r\n");
    ok("OWS trimmed",        "GET /x HTTP/1.1\r\nHost:   c  \t\r\n\r\n");
    bad("space before colon","GET /x HTTP/1.1\r\nHost : c\r\n\r\n", HTTP_ERR_BAD_HEADER);
    bad("no colon",          "GET /x HTTP/1.1\r\nHost c\r\n\r\n", HTTP_ERR_BAD_HEADER);
    bad("empty name",        "GET /x HTTP/1.1\r\n: c\r\nHost: c\r\n\r\n", HTTP_ERR_BAD_HEADER);
    bad("obs-fold",          "GET /x HTTP/1.1\r\nHost: c\r\n continued\r\n\r\n", HTTP_ERR_BAD_HEADER);
    bad("bare LF",           "GET /x HTTP/1.1\nHost: c\r\n\r\n", HTTP_ERR_BAD_HEADER);
    bad("LF-only request",   "GET / HTTP/1.1\nHost: x\n\n", HTTP_ERR_BAD_HEADER);   /* never CRLFCRLF: reject, do not wait */
    bad("leading LF",        "\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", HTTP_ERR_BAD_HEADER);
    {
        /* rejected as soon as the LF arrives, without the rest of the request */
        http_req_t r; size_t c = 0;
        http_req_init(&r);
        CHECK(http_req_feed(&r, (const uint8_t *)"GET / HTTP/1.1\n", 15, &c) == HTTP_REQ_ERROR &&
              http_req_error(&r) == HTTP_ERR_BAD_HEADER && c == 15, "bare LF rejected on arrival (c=%zu)", c);
        http_req_init(&r);
        CHECK(http_req_feed(&r, (const uint8_t *)"GET / HTTP/1.1\r", 15, &c) == HTTP_REQ_NEED_MORE, "CR at chunk end waits");
        CHECK(http_req_feed(&r, (const uint8_t *)"\nHost: x\r\n\r\n", 12, &c) == HTTP_REQ_COMPLETE, "CR|LF split across feeds is fine");
    }
    bad("bare CR",           "GET /x HTTP/1.1\r\nHost: c\rX: y\r\n\r\n", HTTP_ERR_BAD_HEADER);
    bad("ctl in value",      "GET /x HTTP/1.1\r\nHost: c\x01\r\n\r\n", HTTP_ERR_BAD_HEADER);
    bad("DEL in value",      "GET /x HTTP/1.1\r\nHost: c\x7F\r\n\r\n", HTTP_ERR_BAD_HEADER);
    ok("HTAB in value",      "GET /x HTTP/1.1\r\nHost: c\r\nX: a\tb\r\n\r\n");
    ok("obs-text in value",  "GET /x HTTP/1.1\r\nHost: c\r\nX: caf\xC3\xA9\r\n\r\n");
    bad("dup Cookie",        "GET /x HTTP/1.1\r\nHost: c\r\nCookie: a\r\nCookie: b\r\n\r\n", HTTP_ERR_DUP_HEADER);
    bad("dup Authorization", "GET /x HTTP/1.1\r\nHost: c\r\nAuthorization: a\r\nauthorization: b\r\n\r\n", HTTP_ERR_DUP_HEADER);
    bad("dup Content-Type",  "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 0\r\nContent-Type: a\r\nContent-Type: a\r\n\r\n", HTTP_ERR_DUP_HEADER);
    ok("unknown dup fine",   "GET /x HTTP/1.1\r\nHost: c\r\nX-A: 1\r\nX-A: 2\r\n\r\n");
    ok("Upgrade ignored",    "GET /x HTTP/1.1\r\nHost: c\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    bad("Expect",            "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 0\r\nExpect: 100-continue\r\n\r\n", HTTP_ERR_EXPECT);

    /* NUL anywhere in the header block */
    const uint8_t nul[] = "GET /x HTTP/1.1\r\nHost: c\0\r\n\r\n";
    expect_all_splits("NUL in header", nul, sizeof nul - 1, HTTP_REQ_ERROR, HTTP_ERR_BAD_HEADER);
    const uint8_t nul2[] = "GET /x\0 HTTP/1.1\r\nHost: c\r\n\r\n";
    expect_all_splits("NUL in request line", nul2, sizeof nul2 - 1, HTTP_REQ_ERROR, HTTP_ERR_BAD_HEADER);
}

static __attribute__((noinline)) void test_content_length(void)
{
    bad("CL non-digit",      "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 1a\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL negative",       "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: -1\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL plus",           "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: +1\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL empty",          "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length:\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL overflow",       "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 99999999999999999999\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL huge",           "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 4294967296\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL 7 digits big",   "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 9999999\r\n\r\n", HTTP_ERR_BODY_TOO_LARGE);
    bad("CL dup equal",      "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL dup conflict",   "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("CL list",           "POST /x HTTP/1.1\r\nHost: c\r\nContent-Length: 1, 1\r\n\r\n", HTTP_ERR_BAD_CONTENT_LENGTH);
    bad("TE chunked",        "POST /x HTTP/1.1\r\nHost: c\r\nTransfer-Encoding: chunked\r\n\r\n", HTTP_ERR_TE_UNSUPPORTED);
    bad("TE identity",       "POST /x HTTP/1.1\r\nHost: c\r\nTransfer-Encoding: identity\r\n\r\n", HTTP_ERR_TE_UNSUPPORTED);
    bad("TE + CL",           "POST /x HTTP/1.1\r\nHost: c\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\nabc", HTTP_ERR_TE_WITH_CL);
    bad("TE on GET",         "GET /x HTTP/1.1\r\nHost: c\r\nTransfer-Encoding: chunked\r\n\r\n", HTTP_ERR_TE_UNSUPPORTED);
    bad("GET with body",     "GET /x HTTP/1.1\r\nHost: c\r\nContent-Length: 3\r\n\r\nabc", HTTP_ERR_BODY_ON_GET);
    ok("GET with CL 0",      "GET /x HTTP/1.1\r\nHost: c\r\nContent-Length: 0\r\n\r\n");
    bad("OTHER with body",   "PUT /x HTTP/1.1\r\nHost: c\r\nContent-Length: 3\r\n\r\nabc", HTTP_ERR_BODY_ON_GET);
}

static __attribute__((noinline)) void test_reuse_and_state(void)
{
    http_req_t r;
    /* error, then re-init, then a good request on the same context */
    const char *badreq = "GET /x HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n";
    CHECK(feed_plan(&r, (const uint8_t *)badreq, strlen(badreq), NULL, NULL) == HTTP_REQ_ERROR, "bad first");
    const char *good = "GET /api/status HTTP/1.1\r\nHost: c\r\n\r\n";
    CHECK(feed_plan(&r, (const uint8_t *)good, strlen(good), NULL, NULL) == HTTP_REQ_COMPLETE, "good after re-init");
    CHECK(r.header[HTTP_H_COOKIE].len == 0 && r.body_len == 0 && r.err == 0, "no stale state");

    /* feeding after COMPLETE: STATE error, nothing consumed */
    size_t c = 99;
    CHECK(http_req_feed(&r, (const uint8_t *)"GET", 3, &c) == HTTP_REQ_ERROR && c == 0, "feed after complete");
    CHECK(http_req_error(&r) == HTTP_ERR_STATE && http_req_status(&r) == 500, "state error 500");

    /* feeding after ERROR keeps the original error */
    feed_plan(&r, (const uint8_t *)badreq, strlen(badreq), NULL, NULL);
    http_req_feed(&r, (const uint8_t *)"x", 1, &c);
    CHECK(http_req_error(&r) == HTTP_ERR_HOST_DUPLICATE && c == 0, "original error kept");

    /* zero-length feed is harmless */
    http_req_init(&r);
    CHECK(http_req_feed(&r, (const uint8_t *)"", 0, &c) == HTTP_REQ_NEED_MORE && c == 0, "empty feed");

    /* status mapping */
    struct { http_req_err_t e; int s; } map[] = {
        { HTTP_ERR_BAD_REQUEST_LINE, 400 }, { HTTP_ERR_BAD_VERSION, 505 }, { HTTP_ERR_TARGET_TOO_LONG, 414 },
        { HTTP_ERR_HEADER_TOO_LARGE, 431 }, { HTTP_ERR_TE_UNSUPPORTED, 501 }, { HTTP_ERR_LENGTH_REQUIRED, 411 },
        { HTTP_ERR_BODY_TOO_LARGE, 413 }, { HTTP_ERR_EXPECT, 417 }, { HTTP_ERR_HOST_MISSING, 400 },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        http_req_init(&r); r.err = (uint8_t)map[i].e;
        CHECK(http_req_status(&r) == map[i].s, "status for %s", http_req_err_name(map[i].e));
        CHECK(strcmp(http_req_status_text(map[i].s), "Error") != 0, "text for %d", map[i].s);
    }
    CHECK(strcmp(http_req_status_text(504), "Gateway Timeout") == 0, "text for 504 (snapshot)");
}

/* Random bytes, and mutations of a valid request: must terminate with one of
   the three results and never read or write outside the context. The stack
   protector and the sentinel bytes around the context are the only guards
   available on this MinGW (no ASan). */
static __attribute__((noinline)) void test_fuzz(void)
{
    static struct { uint8_t pre[64]; http_req_t r; uint8_t post[64]; } g;
    memset(g.pre, 0xA5, 64); memset(g.post, 0x5A, 64);
    static uint8_t buf[HTTP_REQ_HDR_MAX + HTTP_REQ_BODY_MAX + 128];
    const char *seed = "POST /api/res?v=800x600&r=idle HTTP/1.1\r\nHost: 192.168.11.7\r\nCookie: s=1\r\nContent-Length: 5\r\n\r\nhello";
    int results[3] = {0, 0, 0};

    for (int it = 0; it < 20000; it++) {
        size_t len;
        if (it & 1) {
            len = strlen(seed);
            memcpy(buf, seed, len);
            int muts = 1 + (int)(rnd() % 4);
            for (int m = 0; m < muts; m++) {
                size_t at = rnd() % len;
                switch (rnd() % 3) {
                case 0: buf[at] = (uint8_t)rnd(); break;
                case 1: buf[at] = "\r\n\0 :%?/"[rnd() % 8]; break;
                default: if (len < sizeof buf - 1) { memmove(buf + at + 1, buf + at, len - at); buf[at] = (uint8_t)rnd(); len++; } break;
                }
            }
        } else {
            len = rnd() % sizeof buf;
            for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        }
        http_req_init(&g.r);
        size_t pos = 0;
        http_req_result_t res = HTTP_REQ_NEED_MORE;
        while (pos < len) {
            size_t n = 1 + (rnd() % 97);
            if (n > len - pos) n = len - pos;
            size_t c = 0;
            res = http_req_feed(&g.r, buf + pos, n, &c);
            if (res != HTTP_REQ_NEED_MORE) break;
            pos += n;
        }
        results[res + 1]++;
        if (res == HTTP_REQ_COMPLETE) {
            CHECK(g.r.path_len < HTTP_REQ_PATH_MAX && g.r.path[g.r.path_len] == 0, "path terminated");
            CHECK(g.r.body_len == g.r.content_length && g.r.body_len <= HTTP_REQ_BODY_MAX, "body bounds");
        }
        CHECK(g.r.hdr_len <= HTTP_REQ_HDR_MAX, "hdr_len bound");
    }
    bool guard_ok = true;
    for (int i = 0; i < 64; i++) guard_ok = guard_ok && g.pre[i] == 0xA5 && g.post[i] == 0x5A;
    CHECK(guard_ok, "sentinels around context intact");
    printf("  fuzz: %d error, %d need-more, %d complete\n", results[0], results[1], results[2]);
}

int main(void)
{
    printf("http_req host tests (HDR %d, BODY %d, PATH %d, QUERY %d, sizeof(http_req_t)=%zu)\n",
           HTTP_REQ_HDR_MAX, HTTP_REQ_BODY_MAX, HTTP_REQ_PATH_MAX, HTTP_REQ_QUERY_MAX, sizeof(http_req_t));
    test_basic_get();
    test_existing_routes();
    test_browser_like();
    test_post();
    test_limits();
    test_request_line();
    test_headers();
    test_content_length();
    test_reuse_and_state();
    test_fuzz();
    printf("%s: %d checks passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
