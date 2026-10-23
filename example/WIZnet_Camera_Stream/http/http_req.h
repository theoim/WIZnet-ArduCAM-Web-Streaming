/**
 * @file    http_req.h
 * @brief   Incremental HTTP/1.x request parser for one request per connection.
 *
 * Phase 1a of the HTTPS camera reference work. The server used to read one
 * TLS record into a 1 KB buffer and act on whatever "GET " line it found
 * (server_toe.c, 2026-10). That cannot carry a request split across records,
 * a header block over 1 KB, a Cookie or Authorization header, or a POST body.
 * This parser replaces that: bytes go in as they arrive, in any split, and the
 * handler runs only once the whole request - headers and declared body - is
 * here and has passed the policy below.
 *
 * It depends on nothing but <stdint.h>/<stddef.h>/<string.h>, so the same file
 * compiles on the host (test/http_req_test.c) and on the RP2350.
 *
 * Memory: everything lives inside http_req_t, no allocation. Strings are
 * (offset, length) spans into the request's own header buffer, valid until
 * http_req_init() is called again on that context.
 *
 * One request per connection: once the result is COMPLETE or ERROR the parser
 * consumes no more bytes. Bytes the caller handed in beyond that point are
 * reported through *consumed and are the caller's to discard; the server
 * answers with Connection: close, so a second pipelined request is never
 * served.
 *
 * Policy (default: reject and close). Status in parentheses.
 *   request line   METHOD SP origin-form-target SP HTTP/1.0|1.1 CRLF, single
 *                  spaces, method is a token of 1..16 chars (400). Any method
 *                  token is accepted syntactically; only GET and POST are
 *                  classified, the rest are METHOD_OTHER for the router (405).
 *   version        HTTP/1.0 and HTTP/1.1 only (505)
 *   target         origin-form only ("/..."), pchar set, %XX must be two hex
 *                  digits, no ".." segment, decoded path has no NUL/control
 *                  (400); over the limits (414)
 *   headers        name is a token, no space before ':', no obs-fold, no NUL
 *                  or control chars except HTAB in values, CRLF line ends only
 *                  (400); header block over HTTP_REQ_HDR_MAX (431)
 *   Host           HTTP/1.1: exactly one (400 if missing or duplicated);
 *                  HTTP/1.0: optional, duplicate still 400
 *   Content-Length digits only, no sign, duplicates rejected even if equal
 *                  (400); > HTTP_REQ_BODY_MAX (413); on GET with a non-zero
 *                  value (400); POST without it (411)
 *   Transfer-Enc.  any value (501); together with Content-Length (400)
 *   Expect         any value (417)
 *   Authorization, Cookie, Content-Type: duplicates rejected (400)
 *   Connection     recorded only; the server always closes
 *   Upgrade        ignored; the request is served as plain HTTP/1.1
 *   body           exactly Content-Length bytes, opaque, no text rules
 *   trailing data  after the body is not consumed (no pipelining)
 */

#ifndef HTTP_REQ_H
#define HTTP_REQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef HTTP_REQ_HDR_MAX
#define HTTP_REQ_HDR_MAX    1536    /* request line + headers + CRLFCRLF */
#endif
#ifndef HTTP_REQ_BODY_MAX
#define HTTP_REQ_BODY_MAX   512
#endif
#ifndef HTTP_REQ_PATH_MAX
#define HTTP_REQ_PATH_MAX   256     /* decoded path, excluding the query */
#endif
#ifndef HTTP_REQ_QUERY_MAX
#define HTTP_REQ_QUERY_MAX  512     /* raw query, excluding '?' */
#endif
#define HTTP_REQ_METHOD_MAX 16

typedef enum {
    HTTP_REQ_ERROR     = -1,    /* reject: http_req_status() says how       */
    HTTP_REQ_NEED_MORE =  0,    /* feed more bytes                           */
    HTTP_REQ_COMPLETE  =  1,    /* request validated, run the handler        */
} http_req_result_t;

typedef enum {
    HTTP_METHOD_NONE = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_OTHER,          /* a well-formed token we do not serve       */
} http_method_t;

typedef enum {
    HTTP_ERR_NONE = 0,
    HTTP_ERR_BAD_REQUEST_LINE,  /* 400 */
    HTTP_ERR_BAD_VERSION,       /* 505 */
    HTTP_ERR_BAD_TARGET,        /* 400 */
    HTTP_ERR_TARGET_TOO_LONG,   /* 414 */
    HTTP_ERR_HEADER_TOO_LARGE,  /* 431 */
    HTTP_ERR_BAD_HEADER,        /* 400 */
    HTTP_ERR_HOST_MISSING,      /* 400 */
    HTTP_ERR_HOST_DUPLICATE,    /* 400 */
    HTTP_ERR_DUP_HEADER,        /* 400 */
    HTTP_ERR_BAD_CONTENT_LENGTH,/* 400 */
    HTTP_ERR_TE_UNSUPPORTED,    /* 501 */
    HTTP_ERR_TE_WITH_CL,        /* 400 */
    HTTP_ERR_LENGTH_REQUIRED,   /* 411 */
    HTTP_ERR_BODY_TOO_LARGE,    /* 413 */
    HTTP_ERR_BODY_ON_GET,       /* 400 */
    HTTP_ERR_EXPECT,            /* 417 */
    HTTP_ERR_STATE,             /* 500: fed after COMPLETE/ERROR (caller bug) */
} http_req_err_t;

/* Offset and length into http_req_t.hdr. len 0 and off 0 means "absent". */
typedef struct {
    uint16_t off;
    uint16_t len;
} http_span_t;

typedef enum {
    HTTP_H_HOST = 0,
    HTTP_H_COOKIE,
    HTTP_H_AUTHORIZATION,
    HTTP_H_CONTENT_TYPE,
    HTTP_H_CONNECTION,
    HTTP_H_COUNT_
} http_header_id_t;

typedef struct {
    /* buffers */
    uint8_t  hdr[HTTP_REQ_HDR_MAX];
    uint8_t  body[HTTP_REQ_BODY_MAX];
    uint8_t  path[HTTP_REQ_PATH_MAX];   /* percent-decoded, NUL-terminated */

    uint16_t hdr_len;                   /* bytes stored in hdr               */
    uint16_t hdr_end;                   /* index just past CRLFCRLF, or 0    */
    uint16_t body_len;
    uint16_t content_length;
    uint16_t path_len;

    http_span_t method_span;
    http_span_t query;                  /* raw, into hdr                     */
    http_span_t header[HTTP_H_COUNT_];

    uint8_t  state;                     /* internal                          */
    uint8_t  method;                    /* http_method_t                     */
    uint8_t  err;                       /* http_req_err_t                    */
    uint8_t  version_minor;             /* 0 or 1                            */
    bool     has_content_length;
    bool     has_transfer_encoding;
    bool     has_expect;
    bool     keep_alive_requested;      /* informational only                */
} http_req_t;

/* Reset for a new connection. Wipes buffers (no stale data between uses). */
void http_req_init(http_req_t *r);

/**
 * Feed bytes as they arrive. *consumed receives how many of the len bytes the
 * parser took; the rest were beyond the end of this request (or arrived after
 * an error) and belong to the caller.
 *
 * Returns HTTP_REQ_NEED_MORE while incomplete, HTTP_REQ_COMPLETE once the
 * headers were validated and the declared body is in, HTTP_REQ_ERROR on a
 * policy violation (then http_req_status()). After COMPLETE or ERROR, further
 * calls return HTTP_REQ_ERROR with HTTP_ERR_STATE and consume nothing.
 */
http_req_result_t http_req_feed(http_req_t *r, const uint8_t *data, size_t len,
                                size_t *consumed);

/* True once the header block has been parsed (COMPLETE, or waiting on body). */
bool http_req_headers_done(const http_req_t *r);

/* Accessors. Pointers stay valid until the next http_req_init(). */
http_method_t  http_req_method(const http_req_t *r);
const uint8_t *http_req_method_token(const http_req_t *r, size_t *len);
const char    *http_req_path(const http_req_t *r, size_t *len);   /* decoded */
const uint8_t *http_req_query(const http_req_t *r, size_t *len);  /* raw     */
const uint8_t *http_req_header(const http_req_t *r, http_header_id_t id,
                               size_t *len);                      /* raw, trimmed */
const uint8_t *http_req_body(const http_req_t *r, size_t *len);

/* Error mapping. status is 0 while there is no error. */
http_req_err_t http_req_error(const http_req_t *r);
int            http_req_status(const http_req_t *r);
const char    *http_req_status_text(int status);   /* "Bad Request" etc.    */
const char    *http_req_err_name(http_req_err_t e); /* for logs and tests    */

/**
 * Query helper, the one place that decodes application/x-www-form-urlencoded
 * pairs: finds key in a raw query (q, qlen), decodes %XX and '+' into out
 * (NUL-terminated, at most cap-1 bytes). Returns the decoded length, -1 if the
 * key is absent, -2 if the value does not fit or has a bad escape.
 */
int http_query_get(const uint8_t *q, size_t qlen, const char *key,
                   char *out, size_t cap);

#endif /* HTTP_REQ_H */
