/**
 * @file    http_server.h
 * @brief   HTTP on top of wiz_transport: request intake, route lookup,
 *          authorization hook, responses. One request per connection.
 *
 * Step 2 (2026-11-05): implemented in http_server.c; server_toe.c holds the
 * routes and handlers until the stream and auth parts move out (steps 3-4).
 *
 * The application supplies route tables and an authorization function; this
 * module knows neither cameras nor accounts. A handler gets an http_ctx_t
 * for its request and answers through the http_reply_* helpers - it never
 * sees a socket, and nothing about the request is kept in globals.
 *
 * Order for every complete request (unchanged from server_toe.c):
 *   path not found          -> 404
 *   method not allowed      -> 405 with Allow:
 *   authorize() != 0        -> 401 / 403, JSON body with the reason
 *   handler                 -> response; the connection closes after it,
 *                              unless the handler handed it on (mjpeg_begin)
 *
 * Lifetime: an http_ctx_t and its req are valid only while the handler runs.
 * Nothing may keep either pointer: after the handler the request slot is
 * reused, and a connection handed to mjpeg_begin() gives its slot back at
 * once (wt_hold). Copy out what must outlive the handler.
 *
 * Blocking: the http_reply_* helpers send with wt_send_all() - bounded
 * blocking, up to 2 s without progress (see wiz_transport.h).
 *
 * Memory: one http_ctx_t per request slot (WT_MAX_REQUEST_SLOTS), static.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "http_req.h"
#include "wiz_transport.h"

#define HTTP_M_GET   (1u << HTTP_METHOD_GET)
#define HTTP_M_POST  (1u << HTTP_METHOD_POST)

typedef struct http_ctx {
    wt_conn_t         conn;
    const http_req_t *req;
    void             *app;      /* from http_server_config_t */
    const void       *auth;     /* set by authorize(): the caller's session,
                                   or NULL. Opaque here. */
} http_ctx_t;

/* >= 0 handled (response sent or connection handed on), < 0 send failed:
 * the connection is closed. */
typedef int32_t (*http_handler_t)(http_ctx_t *ctx);

typedef struct {
    const char     *path;       /* exact match on the decoded path */
    uint8_t         methods;    /* HTTP_M_GET | HTTP_M_POST */
    uint8_t         access;     /* meaning belongs to authorize(), e.g.
                                   AUTH_ACCESS_* in auth_http.h */
    http_handler_t  fn;
} http_route_t;

typedef struct {
    const http_route_t *routes;
    size_t              count;
} http_route_table_t;

/* 0 to serve; else 401 or 403 with *why for the body. May set ctx->auth.
 * NULL means everything is allowed. */
typedef int (*http_authorize_fn)(void *app, const http_route_t *rt,
                                 http_ctx_t *ctx, const char **why);

typedef struct {
    const http_route_table_t *tables;   /* searched in order */
    size_t                    n_tables;
    http_authorize_fn         authorize;
    void                     *app;
    bool                      log_requests;   /* "[req sN] /path" */
} http_server_config_t;

/* Fills *out_handlers for wt_init(): on_ready, on_readable,
 * on_request_timeout, on_refused. on_hold and on_closed are left NULL for
 * whoever holds connections (the MJPEG stream) to fill in. Call before
 * wt_init() - see the start-up order in wiz_transport.h. The tables and cfg
 * must stay valid (static). */
void    http_server_init(const http_server_config_t *cfg, wt_handlers_t *out_handlers);

/* ---- responses: all send Connection: close and Cache-Control: no-store -- */

/* Status line, Content-Type, Content-Length, optional extra header lines
 * ("Name: value\r\n" each), body. */
int32_t http_reply(http_ctx_t *ctx, const char *status, const char *ctype,
                   const char *extra, const void *body, uint32_t len);

/* Head only, for a body sent in pieces afterwards with wt_send_all(ctx->conn,
 * ...); len is the whole body. */
int32_t http_reply_head(http_ctx_t *ctx, const char *status, const char *ctype,
                        const char *extra, uint32_t len);

/* "404 Not Found" etc. with the reason phrase as a text/plain body. */
int32_t http_reply_error(http_ctx_t *ctx, int code, const char *extra);

/* {"error":"<msg>"[,<detail_json>]} with the given status. detail_json is
 * extra members without braces or leading comma, e.g. "\"control\":\"flip\"",
 * or NULL. */
int32_t http_reply_json_error(http_ctx_t *ctx, const char *status,
                              const char *extra, const char *msg,
                              const char *detail_json);

/* ---- request values: decoded once by the parser ---------------------- */

size_t  http_query(const http_ctx_t *ctx, const char *key, char *out, size_t cap);
bool    http_query_u8(const http_ctx_t *ctx, const char *key, uint8_t *out);
/* application/x-www-form-urlencoded body value. */
size_t  http_form(const http_ctx_t *ctx, const char *key, char *out, size_t cap);
bool    http_is_form(const http_ctx_t *ctx);

#endif /* HTTP_SERVER_H */
