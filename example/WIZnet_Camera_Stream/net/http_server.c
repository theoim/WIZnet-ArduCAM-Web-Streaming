/**
 * @file    http_server.c
 * @brief   HTTP on top of wiz_transport. See http_server.h.
 *
 * Step 2 of the server_toe.c split (2026-11-05). Request intake, route lookup,
 * the 404/405/401/403 order and the response helpers moved here from
 * server_toe.c unchanged in behaviour. Log lines keep their old "s<N>" form:
 * N is the connection index, which equals the socket number while
 * SOCK_HTTP_BASE is 0.
 */

#include "http_server.h"

#include <stdio.h>
#include <string.h>

#define HTTP_RX_CHUNK       512         /* bytes read per receive call          */
#define HTTP_RX_PASSES      4           /* receive calls per socket per pass    */

/*
 * The request contexts, one per transport request slot. A request accumulates
 * in the context its connection owns until it is complete, however the bytes
 * were split. g_rx is only a staging buffer for one receive call; nothing in
 * it outlives the http_req_feed() that copies it.
 */
static http_req_t                  g_req[WT_MAX_REQUEST_SLOTS];
static http_ctx_t                  g_ctx[WT_MAX_REQUEST_SLOTS];
static uint8_t                     g_rx[HTTP_RX_CHUNK];
static char                        g_tx_hdr[256];
static const http_server_config_t *g_cfg;

/* ------------------------------- Responses -------------------------------- */
int32_t http_reply_head(http_ctx_t *ctx, const char *status, const char *ctype,
                        const char *extra, uint32_t len)
{
    int n = snprintf(g_tx_hdr, sizeof(g_tx_hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-store\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     status, ctype, (unsigned long)len, extra ? extra : "");
    if (n <= 0 || n >= (int)sizeof(g_tx_hdr)) {
        return -1;
    }
    if (wt_send_all(ctx->conn, g_tx_hdr, (uint32_t)n) < 0) {
        return -1;
    }
    return 1;
}

int32_t http_reply(http_ctx_t *ctx, const char *status, const char *ctype,
                   const char *extra, const void *body, uint32_t len)
{
    if (http_reply_head(ctx, status, ctype, extra, len) < 0) {
        return -1;
    }
    if (len && wt_send_all(ctx->conn, body, len) < 0) {
        return -1;
    }
    return 1;
}

int32_t http_reply_error(http_ctx_t *ctx, int code, const char *extra)
{
    char status[48];
    const char *text = http_req_status_text(code);
    snprintf(status, sizeof(status), "%d %s", code, text);
    return http_reply(ctx, status, "text/plain", extra, text, (uint32_t)strlen(text));
}

int32_t http_reply_json_error(http_ctx_t *ctx, const char *status,
                              const char *extra, const char *msg,
                              const char *detail_json)
{
    char body[160];
    int n;
    if (detail_json && detail_json[0]) {
        n = snprintf(body, sizeof(body), "{\"error\":\"%s\",%s}", msg, detail_json);
    } else {
        n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    }
    if (n < 0 || (size_t)n >= sizeof(body)) {
        n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    }
    return http_reply(ctx, status, "application/json", extra, body, (uint32_t)n);
}

/* ---------------------------- Request values ------------------------------ */
size_t http_query(const http_ctx_t *ctx, const char *key, char *out, size_t cap)
{
    size_t qlen;
    const uint8_t *q = http_req_query(ctx->req, &qlen);
    int n = http_query_get(q, qlen, key, out, cap);
    if (n < 0) {
        out[0] = 0;
        return 0;
    }
    return (size_t)n;
}

/* Strict small unsigned: 1..3 digits, value <= 255. */
bool http_query_u8(const http_ctx_t *ctx, const char *key, uint8_t *out)
{
    char v[8];
    size_t n = http_query(ctx, key, v, sizeof(v));
    if (n == 0 || n > 3) return false;
    unsigned x = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return false;
        x = x * 10u + (unsigned)(v[i] - '0');
    }
    if (x > 255u) return false;
    *out = (uint8_t)x;
    return true;
}

size_t http_form(const http_ctx_t *ctx, const char *key, char *out, size_t cap)
{
    size_t blen = 0;
    const uint8_t *b = http_req_body(ctx->req, &blen);
    int n = http_query_get(b, blen, key, out, cap);
    if (n < 0) {
        out[0] = 0;
        return 0;
    }
    return (size_t)n;
}

/* Content-Type must be form-urlencoded; parameters after ';' are allowed. */
bool http_is_form(const http_ctx_t *ctx)
{
    static const char want[] = "application/x-www-form-urlencoded";
    size_t len;
    const uint8_t *v = http_req_header(ctx->req, HTTP_H_CONTENT_TYPE, &len);
    size_t wl = sizeof(want) - 1;
    if (!v || len < wl) return false;
    for (size_t i = 0; i < wl; i++) {
        uint8_t c = v[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        if (c != (uint8_t)want[i]) return false;
    }
    return len == wl || v[wl] == ';' || v[wl] == ' ';
}

/* --------------------------------- Routing -------------------------------- */
static const http_route_t *route_find(const char *path, size_t len)
{
    for (size_t t = 0; t < g_cfg->n_tables; t++) {
        const http_route_table_t *tb = &g_cfg->tables[t];
        for (size_t i = 0; i < tb->count; i++) {
            size_t n = strlen(tb->routes[i].path);
            if (n == len && memcmp(tb->routes[i].path, path, n) == 0) {
                return &tb->routes[i];
            }
        }
    }
    return NULL;
}

static void allow_header(const http_route_t *rt, char *out, size_t cap)
{
    snprintf(out, cap, "Allow: %s%s%s\r\n",
             (rt->methods & HTTP_M_GET) ? "GET" : "",
             ((rt->methods & HTTP_M_GET) && (rt->methods & HTTP_M_POST)) ? ", " : "",
             (rt->methods & HTTP_M_POST) ? "POST" : "");
}

/* A complete, validated request: look it up, check it, run it, end it. */
static void dispatch(http_ctx_t *ctx)
{
    size_t plen;
    const char *path = http_req_path(ctx->req, &plen);
    if (g_cfg->log_requests) {
        printf("[req s%u] %.*s\n", ctx->conn, (int)plen, path);
    }
    const http_route_t *rt = route_find(path, plen);
    int32_t rc;
    int deny = 0;
    const char *why = "";

    if (!rt) {
        rc = http_reply_error(ctx, 404, NULL);
    } else if (!(rt->methods & (1u << http_req_method(ctx->req)))) {
        char allow[32];
        allow_header(rt, allow, sizeof(allow));
        rc = http_reply_error(ctx, 405, allow);
    } else if (g_cfg->authorize &&
               (deny = g_cfg->authorize(g_cfg->app, rt, ctx, &why)) != 0) {
        rc = http_reply_json_error(ctx, deny == 401 ? "401 Unauthorized" : "403 Forbidden",
                                   NULL, why, NULL);
    } else {
        rc = rt->fn(ctx);
    }

    if (rc < 0) {
        wt_close(ctx->conn, WT_CLOSE_SEND);
    } else if (!wt_is_held(ctx->conn)) {
        wt_close(ctx->conn, WT_CLOSE_DONE);         /* Connection: close */
    }
}

/* --------------------------- Transport callbacks -------------------------- */
static http_ctx_t *ctx_for(wt_conn_t c, wt_slot_t slot)
{
    http_ctx_t *ctx = &g_ctx[slot];
    ctx->conn = c;
    ctx->req  = &g_req[slot];
    ctx->app  = g_cfg->app;
    ctx->auth = NULL;
    return ctx;
}

static void on_ready(void *user, wt_conn_t c, wt_slot_t slot)
{
    (void)user;
    (void)c;
    http_req_init(&g_req[slot]);
}

/* Read what has arrived into this connection's request context. */
static void on_readable(void *user, wt_conn_t c, wt_slot_t slot)
{
    (void)user;
    http_req_t *r = &g_req[slot];

    for (int pass = 0; pass < HTTP_RX_PASSES; pass++) {
        int32_t n = wt_recv(c, g_rx, sizeof(g_rx));
        if (n < 0) {
            wt_close(c, WT_CLOSE_PEER);             /* FIN/RST, close_notify or TLS error */
            return;
        }
        if (n == 0) {
            return;                                 /* nothing more this pass */
        }

        size_t used = 0;
        http_req_result_t res = http_req_feed(r, g_rx, (size_t)n, &used);
        if (res == HTTP_REQ_NEED_MORE) {
            continue;
        }
        if (res == HTTP_REQ_ERROR) {
            int st = http_req_status(r);
            printf("[http] s%u rejected: %d %s\n", c, st,
                   http_req_err_name(http_req_error(r)));
            http_reply_error(ctx_for(c, slot), st, NULL);
            wt_close(c, WT_CLOSE_PARSE);
            return;
        }
        /* COMPLETE. Anything after the request in g_rx (a pipelined second
           request) is dropped: one request per connection. */
        dispatch(ctx_for(c, slot));
        return;
    }
}

static void on_request_timeout(void *user, wt_conn_t c, wt_slot_t slot)
{
    (void)user;
    http_reply_error(ctx_for(c, slot), 408, NULL);
}

static void on_refused(void *user, wt_conn_t c)
{
    (void)user;
    /* No slot, so no context of its own: a stack one that carries no request. */
    http_ctx_t ctx = { .conn = c, .req = NULL, .app = g_cfg->app, .auth = NULL };
    http_reply_error(&ctx, 503, NULL);
}

void http_server_init(const http_server_config_t *cfg, wt_handlers_t *out)
{
    g_cfg = cfg;
    memset(out, 0, sizeof(*out));
    out->on_ready           = on_ready;
    out->on_readable        = on_readable;
    out->on_request_timeout = on_request_timeout;
    out->on_refused         = on_refused;
    out->user               = NULL;
}
