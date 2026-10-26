/**
 * @file    http_req.c
 * @brief   Incremental HTTP/1.x request parser - see http_req.h for the policy.
 *
 * Shape: bytes are appended to r->hdr until CRLFCRLF appears, then the whole
 * header block is parsed and validated in one pass (parse_headers). Bytes that
 * arrived after the terminator in the same feed are body bytes and go straight
 * to r->body. Keeping the header block whole and parsing it once is simpler
 * than a per-character state machine and the buffer is only 1.5 KB, so the
 * cost of re-scanning is nothing.
 */

#include <string.h>

#include "http_req.h"

enum {
    ST_HEADERS = 0,     /* collecting the header block                       */
    ST_BODY,            /* headers parsed, collecting content_length bytes   */
    ST_DONE,            /* COMPLETE - no more input accepted                 */
    ST_ERROR,
};

/* ------------------------------------------------------------- character sets */

/* RFC 9110 token: !#$%&'*+-.^_`|~ DIGIT ALPHA */
static bool is_tchar(uint8_t c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

/* pchar without pct-encoded: unreserved / sub-delims / ":" / "@" */
static bool is_pchar(uint8_t c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    switch (c) {
    case '-': case '.': case '_': case '~':                     /* unreserved */
    case '!': case '$': case '&': case '\'': case '(': case ')':
    case '*': case '+': case ',': case ';': case '=':           /* sub-delims */
    case ':': case '@':
        return true;
    default:
        return false;
    }
}

static int hexval(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ieq(const uint8_t *a, size_t alen, const char *lit)
{
    size_t n = strlen(lit);
    if (alen != n) return false;
    for (size_t i = 0; i < n; i++) {
        uint8_t x = a[i], y = (uint8_t)lit[i];
        if (x >= 'A' && x <= 'Z') x = (uint8_t)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (uint8_t)(y + 32);
        if (x != y) return false;
    }
    return true;
}

static http_req_result_t fail(http_req_t *r, http_req_err_t e)
{
    r->err   = (uint8_t)e;
    r->state = ST_ERROR;
    return HTTP_REQ_ERROR;
}

/* ---------------------------------------------------------------- init/API */

void http_req_init(http_req_t *r)
{
    memset(r, 0, sizeof(*r));
    r->state = ST_HEADERS;
}

bool http_req_headers_done(const http_req_t *r)
{
    return r->state == ST_BODY || r->state == ST_DONE;
}

http_method_t http_req_method(const http_req_t *r) { return (http_method_t)r->method; }

const uint8_t *http_req_method_token(const http_req_t *r, size_t *len)
{
    *len = r->method_span.len;
    return r->hdr + r->method_span.off;
}

const char *http_req_path(const http_req_t *r, size_t *len)
{
    *len = r->path_len;
    return (const char *)r->path;
}

const uint8_t *http_req_query(const http_req_t *r, size_t *len)
{
    *len = r->query.len;
    return r->hdr + r->query.off;
}

const uint8_t *http_req_header(const http_req_t *r, http_header_id_t id, size_t *len)
{
    if ((unsigned)id >= HTTP_H_COUNT_ || r->header[id].len == 0) {
        *len = 0;
        return NULL;
    }
    *len = r->header[id].len;
    return r->hdr + r->header[id].off;
}

const uint8_t *http_req_body(const http_req_t *r, size_t *len)
{
    *len = r->body_len;
    return r->body;
}

http_req_err_t http_req_error(const http_req_t *r) { return (http_req_err_t)r->err; }

int http_req_status(const http_req_t *r)
{
    switch ((http_req_err_t)r->err) {
    case HTTP_ERR_NONE:               return 0;
    case HTTP_ERR_BAD_VERSION:        return 505;
    case HTTP_ERR_TARGET_TOO_LONG:    return 414;
    case HTTP_ERR_HEADER_TOO_LARGE:   return 431;
    case HTTP_ERR_TE_UNSUPPORTED:     return 501;
    case HTTP_ERR_LENGTH_REQUIRED:    return 411;
    case HTTP_ERR_BODY_TOO_LARGE:     return 413;
    case HTTP_ERR_EXPECT:             return 417;
    case HTTP_ERR_STATE:              return 500;
    default:                          return 400;
    }
}

const char *http_req_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Error";
    }
}

const char *http_req_err_name(http_req_err_t e)
{
    switch (e) {
    case HTTP_ERR_NONE:                return "none";
    case HTTP_ERR_BAD_REQUEST_LINE:    return "bad-request-line";
    case HTTP_ERR_BAD_VERSION:         return "bad-version";
    case HTTP_ERR_BAD_TARGET:          return "bad-target";
    case HTTP_ERR_TARGET_TOO_LONG:     return "target-too-long";
    case HTTP_ERR_HEADER_TOO_LARGE:    return "header-too-large";
    case HTTP_ERR_BAD_HEADER:          return "bad-header";
    case HTTP_ERR_HOST_MISSING:        return "host-missing";
    case HTTP_ERR_HOST_DUPLICATE:      return "host-duplicate";
    case HTTP_ERR_DUP_HEADER:          return "duplicate-header";
    case HTTP_ERR_BAD_CONTENT_LENGTH:  return "bad-content-length";
    case HTTP_ERR_TE_UNSUPPORTED:      return "transfer-encoding-unsupported";
    case HTTP_ERR_TE_WITH_CL:          return "transfer-encoding-with-content-length";
    case HTTP_ERR_LENGTH_REQUIRED:     return "length-required";
    case HTTP_ERR_BODY_TOO_LARGE:      return "body-too-large";
    case HTTP_ERR_BODY_ON_GET:         return "body-on-get";
    case HTTP_ERR_EXPECT:              return "expect";
    case HTTP_ERR_STATE:               return "fed-after-done";
    default:                           return "?";
    }
}

/* ------------------------------------------------------------ request line */

/* Validate and decode the origin-form target at hdr[off..off+len). */
static http_req_err_t parse_target(http_req_t *r, uint16_t off, uint16_t len)
{
    const uint8_t *t = r->hdr + off;
    if (len == 0 || t[0] != '/') return HTTP_ERR_BAD_TARGET;

    /* split at the first '?' */
    uint16_t plen = len, qoff = 0, qlen = 0;
    for (uint16_t i = 0; i < len; i++) {
        if (t[i] == '?') {
            plen = i;
            qoff = (uint16_t)(off + i + 1);
            qlen = (uint16_t)(len - i - 1);
            break;
        }
    }
    if (qlen > HTTP_REQ_QUERY_MAX) return HTTP_ERR_TARGET_TOO_LONG;

    /* query: pchar / "/" / "?" / pct-encoded, validated but kept raw */
    for (uint16_t i = 0; i < qlen; i++) {
        uint8_t c = r->hdr[qoff + i];
        if (c == '%') {
            if ((uint32_t)i + 2u >= qlen) return HTTP_ERR_BAD_TARGET;   /* need two hex digits */
            if (hexval(r->hdr[qoff + i + 1]) < 0 || hexval(r->hdr[qoff + i + 2]) < 0) {
                return HTTP_ERR_BAD_TARGET;
            }
            i = (uint16_t)(i + 2u);
        } else if (!(is_pchar(c) || c == '/' || c == '?')) {
            return HTTP_ERR_BAD_TARGET;
        }
    }
    r->query.off = qoff;
    r->query.len = qlen;

    /* path: decode into r->path, reject control chars and ".." segments */
    uint16_t out = 0;
    uint16_t seg_start = 0;             /* index in r->path of current segment */
    for (uint16_t i = 0; i < plen; i++) {
        uint8_t c = t[i];
        uint8_t d;
        if (c == '%') {
            if (i + 2 >= plen) return HTTP_ERR_BAD_TARGET;
            int h = hexval(t[i + 1]), l = hexval(t[i + 2]);
            if (h < 0 || l < 0) return HTTP_ERR_BAD_TARGET;
            d = (uint8_t)((h << 4) | l);
            i = (uint16_t)(i + 2u);
        } else if (is_pchar(c) || c == '/') {
            d = c;
        } else {
            return HTTP_ERR_BAD_TARGET;
        }
        if (d < 0x20 || d == 0x7F) return HTTP_ERR_BAD_TARGET;   /* NUL, controls */
        if (out + 1 >= HTTP_REQ_PATH_MAX) return HTTP_ERR_TARGET_TOO_LONG;
        if (d == '/') {
            uint16_t seg = (uint16_t)(out - seg_start);
            if (seg == 2 && r->path[seg_start] == '.' && r->path[seg_start + 1] == '.') {
                return HTTP_ERR_BAD_TARGET;
            }
            seg_start = (uint16_t)(out + 1);
        }
        r->path[out++] = d;
    }
    {
        uint16_t seg = (uint16_t)(out - seg_start);
        if (seg == 2 && r->path[seg_start] == '.' && r->path[seg_start + 1] == '.') {
            return HTTP_ERR_BAD_TARGET;
        }
    }
    r->path[out]  = 0;
    r->path_len   = out;
    return HTTP_ERR_NONE;
}

/* "METHOD SP target SP HTTP/1.x" - line is hdr[0..line_len) without CRLF */
static http_req_err_t parse_request_line(http_req_t *r, uint16_t line_len)
{
    const uint8_t *p = r->hdr;
    uint16_t i = 0;

    while (i < line_len && is_tchar(p[i])) i++;
    if (i == 0 || i > HTTP_REQ_METHOD_MAX) return HTTP_ERR_BAD_REQUEST_LINE;
    if (i >= line_len || p[i] != ' ')      return HTTP_ERR_BAD_REQUEST_LINE;
    r->method_span.off = 0;
    r->method_span.len = i;
    if (ieq(p, i, "GET"))       r->method = HTTP_METHOD_GET;
    else if (ieq(p, i, "POST")) r->method = HTTP_METHOD_POST;
    else                        r->method = HTTP_METHOD_OTHER;

    uint16_t toff = (uint16_t)(i + 1);
    uint16_t j = toff;
    while (j < line_len && p[j] != ' ') {
        if (p[j] < 0x21 || p[j] == 0x7F) return HTTP_ERR_BAD_REQUEST_LINE; /* no ctl/SP/HTAB in target */
        j++;
    }
    if (j == toff || j >= line_len) return HTTP_ERR_BAD_REQUEST_LINE;
    uint16_t tlen = (uint16_t)(j - toff);

    /* version: exactly "HTTP/1.0" or "HTTP/1.1" to end of line */
    uint16_t v = (uint16_t)(j + 1);
    if (line_len - v != 8) return HTTP_ERR_BAD_REQUEST_LINE;
    if (memcmp(p + v, "HTTP/", 5) != 0) return HTTP_ERR_BAD_REQUEST_LINE;
    if (p[v + 5] < '0' || p[v + 5] > '9' || p[v + 6] != '.' ||
        p[v + 7] < '0' || p[v + 7] > '9') {
        return HTTP_ERR_BAD_REQUEST_LINE;
    }
    if (p[v + 5] != '1' || (p[v + 7] != '0' && p[v + 7] != '1')) return HTTP_ERR_BAD_VERSION;
    r->version_minor = (uint8_t)(p[v + 7] - '0');

    if (tlen > HTTP_REQ_PATH_MAX + 1 + HTTP_REQ_QUERY_MAX) return HTTP_ERR_TARGET_TOO_LONG;
    return parse_target(r, toff, tlen);
}

/* ----------------------------------------------------------------- headers */

static http_req_err_t set_once(http_req_t *r, http_header_id_t id, uint16_t off,
                               uint16_t len, http_req_err_t dup_err)
{
    if (r->header[id].len != 0) return dup_err;
    r->header[id].off = off;
    r->header[id].len = len;
    return HTTP_ERR_NONE;
}

static http_req_err_t parse_content_length(http_req_t *r, const uint8_t *v, uint16_t vlen)
{
    if (r->has_content_length) return HTTP_ERR_BAD_CONTENT_LENGTH;   /* duplicate, even if equal */
    if (vlen == 0 || vlen > 7)  return HTTP_ERR_BAD_CONTENT_LENGTH;   /* 7 digits caps at 9,999,999 */
    uint32_t n = 0;
    for (uint16_t i = 0; i < vlen; i++) {
        if (v[i] < '0' || v[i] > '9') return HTTP_ERR_BAD_CONTENT_LENGTH;
        n = n * 10u + (uint32_t)(v[i] - '0');
    }
    r->has_content_length = true;
    if (n > HTTP_REQ_BODY_MAX) return HTTP_ERR_BODY_TOO_LARGE;
    r->content_length = (uint16_t)n;
    return HTTP_ERR_NONE;
}

static http_req_err_t parse_headers(http_req_t *r)
{
    const uint8_t *h   = r->hdr;
    uint16_t       end = r->hdr_end;        /* just past CRLFCRLF */
    uint16_t       pos = 0;
    bool           first = true;

    /* Any NUL anywhere in the header block is a hard reject. */
    for (uint16_t i = 0; i < end; i++) {
        if (h[i] == 0) return HTTP_ERR_BAD_HEADER;
    }

    while (pos < end) {
        /* find CRLF; a bare LF or a bare CR inside a line is a reject */
        uint16_t eol = pos;
        while (eol + 1 < end && !(h[eol] == '\r' && h[eol + 1] == '\n')) {
            if (h[eol] == '\r' || h[eol] == '\n') return HTTP_ERR_BAD_HEADER;
            eol++;
        }
        if (eol + 1 >= end) return HTTP_ERR_BAD_HEADER;    /* cannot happen: block ends in CRLF */
        uint16_t line_len = (uint16_t)(eol - pos);

        if (first) {
            http_req_err_t e = parse_request_line(r, line_len);
            if (e != HTTP_ERR_NONE) return e;
            first = false;
            pos = (uint16_t)(eol + 2);
            continue;
        }
        if (line_len == 0) break;                           /* the empty line */

        const uint8_t *ln = h + pos;
        if (ln[0] == ' ' || ln[0] == '\t') return HTTP_ERR_BAD_HEADER;   /* obs-fold */

        uint16_t n = 0;
        while (n < line_len && is_tchar(ln[n])) n++;
        if (n == 0 || n >= line_len || ln[n] != ':') return HTTP_ERR_BAD_HEADER;

        /* value: skip OWS, trim trailing OWS, reject controls other than HTAB */
        uint16_t vs = (uint16_t)(n + 1);
        while (vs < line_len && (ln[vs] == ' ' || ln[vs] == '\t')) vs++;
        uint16_t ve = line_len;
        while (ve > vs && (ln[ve - 1] == ' ' || ln[ve - 1] == '\t')) ve--;
        for (uint16_t i = vs; i < ve; i++) {
            if ((ln[i] < 0x20 && ln[i] != '\t') || ln[i] == 0x7F) return HTTP_ERR_BAD_HEADER;
        }
        uint16_t voff = (uint16_t)(pos + vs), vlen = (uint16_t)(ve - vs);
        http_req_err_t e = HTTP_ERR_NONE;

        if (ieq(ln, n, "Host")) {
            e = set_once(r, HTTP_H_HOST, voff, vlen == 0 ? 1 : vlen, HTTP_ERR_HOST_DUPLICATE);
            if (e == HTTP_ERR_NONE && vlen == 0) r->header[HTTP_H_HOST].len = 1; /* present, empty: still counts */
        } else if (ieq(ln, n, "Content-Length")) {
            e = parse_content_length(r, ln + vs, vlen);
        } else if (ieq(ln, n, "Transfer-Encoding")) {
            r->has_transfer_encoding = true;
        } else if (ieq(ln, n, "Expect")) {
            r->has_expect = true;
        } else if (ieq(ln, n, "Cookie")) {
            e = set_once(r, HTTP_H_COOKIE, voff, vlen ? vlen : 1, HTTP_ERR_DUP_HEADER);
        } else if (ieq(ln, n, "Authorization")) {
            e = set_once(r, HTTP_H_AUTHORIZATION, voff, vlen ? vlen : 1, HTTP_ERR_DUP_HEADER);
        } else if (ieq(ln, n, "Content-Type")) {
            e = set_once(r, HTTP_H_CONTENT_TYPE, voff, vlen ? vlen : 1, HTTP_ERR_DUP_HEADER);
        } else if (ieq(ln, n, "Connection")) {
            r->header[HTTP_H_CONNECTION].off = voff;
            r->header[HTTP_H_CONNECTION].len = vlen ? vlen : 1;
            r->keep_alive_requested = ieq(ln + vs, vlen, "keep-alive");
        }
        /* Upgrade and everything else: ignored */
        if (e != HTTP_ERR_NONE) return e;
        pos = (uint16_t)(eol + 2);
    }

    /* cross-header policy */
    if (r->version_minor == 1 && r->header[HTTP_H_HOST].len == 0) return HTTP_ERR_HOST_MISSING;
    if (r->has_transfer_encoding) {
        return r->has_content_length ? HTTP_ERR_TE_WITH_CL : HTTP_ERR_TE_UNSUPPORTED;
    }
    if (r->has_expect) return HTTP_ERR_EXPECT;
    if (r->method == HTTP_METHOD_POST && !r->has_content_length) return HTTP_ERR_LENGTH_REQUIRED;
    if (r->method != HTTP_METHOD_POST && r->content_length != 0) return HTTP_ERR_BODY_ON_GET;
    return HTTP_ERR_NONE;
}

/* --------------------------------------------------------------------- feed */

http_req_result_t http_req_feed(http_req_t *r, const uint8_t *data, size_t len,
                                size_t *consumed)
{
    size_t used = 0;
    if (consumed) *consumed = 0;

    if (r->state == ST_DONE || r->state == ST_ERROR) {
        if (r->err == HTTP_ERR_NONE) r->err = HTTP_ERR_STATE;
        return HTTP_REQ_ERROR;
    }

    if (r->state == ST_HEADERS) {
        /* append, then look for the terminator starting a little before the
           old end so a CRLFCRLF split across feeds is still seen */
        size_t room = (size_t)HTTP_REQ_HDR_MAX - r->hdr_len;
        size_t take = len < room ? len : room;
        uint16_t scan_from = r->hdr_len > 3 ? (uint16_t)(r->hdr_len - 3) : 0;
        memcpy(r->hdr + r->hdr_len, data, take);
        r->hdr_len = (uint16_t)(r->hdr_len + take);

        /* A bare LF can never be part of a valid header block, and a client
           that ends lines with LF alone never produces the CRLFCRLF this loop
           waits for - it would sit on a connection until the request timeout
           (measured on the board 2026-10-23: 408 after 5 s). Reject on sight. */
        {
            uint16_t from = r->hdr_len > take ? (uint16_t)(r->hdr_len - take) : 0;
            for (uint16_t i = from; i < r->hdr_len; i++) {
                if (r->hdr[i] == '\n' && (i == 0 || r->hdr[i - 1] != '\r')) {
                    if (consumed) *consumed = (size_t)(i + 1u - from);
                    return fail(r, HTTP_ERR_BAD_HEADER);
                }
            }
        }

        uint16_t term = 0;
        bool found = false;
        for (uint16_t i = scan_from; i + 3 < r->hdr_len; i++) {
            if (r->hdr[i] == '\r' && r->hdr[i + 1] == '\n' &&
                r->hdr[i + 2] == '\r' && r->hdr[i + 3] == '\n') {
                term  = (uint16_t)(i + 4);
                found = true;
                break;
            }
        }
        if (!found) {
            if (r->hdr_len >= HTTP_REQ_HDR_MAX) {
                if (consumed) *consumed = take;
                return fail(r, HTTP_ERR_HEADER_TOO_LARGE);
            }
            if (consumed) *consumed = take;          /* take == len here */
            return HTTP_REQ_NEED_MORE;
        }

        /* bytes after the terminator were copied into hdr; hand them back as
           the start of the body */
        uint16_t extra = (uint16_t)(r->hdr_len - term);
        used = take - extra;                          /* header bytes consumed from this feed */
        r->hdr_len = term;
        r->hdr_end = term;

        http_req_err_t e = parse_headers(r);
        if (e != HTTP_ERR_NONE) {
            if (consumed) *consumed = used;
            return fail(r, e);
        }
        r->state = ST_BODY;
        /* fall through to body handling with the leftover of this feed */
        data += used;
        len  -= used;
    }

    /* ST_BODY */
    {
        size_t need = (size_t)r->content_length - r->body_len;
        size_t take = len < need ? len : need;
        if (take) {
            memcpy(r->body + r->body_len, data, take);
            r->body_len = (uint16_t)(r->body_len + take);
        }
        used += take;
        if (consumed) *consumed = used;
        if (r->body_len == r->content_length) {
            r->state = ST_DONE;
            return HTTP_REQ_COMPLETE;
        }
        return HTTP_REQ_NEED_MORE;
    }
}

/* -------------------------------------------------------------------- query */

int http_query_get(const uint8_t *q, size_t qlen, const char *key,
                   char *out, size_t cap)
{
    size_t klen = strlen(key);
    size_t i = 0;
    if (cap == 0) return -2;

    while (i < qlen) {
        /* key at i? */
        size_t j = i;
        while (j < qlen && q[j] != '&' && q[j] != '=') j++;
        bool match = (j - i == klen) && memcmp(q + i, key, klen) == 0;
        if (match) {
            size_t vs = (j < qlen && q[j] == '=') ? j + 1 : j;
            size_t ve = vs;
            while (ve < qlen && q[ve] != '&') ve++;
            size_t o = 0;
            for (size_t k = vs; k < ve; k++) {
                uint8_t c = q[k];
                if (c == '%') {
                    if (k + 2 >= ve) return -2;                     /* need two hex digits */
                    int h = hexval(q[k + 1]), l = hexval(q[k + 2]);
                    if (h < 0 || l < 0) return -2;
                    c = (uint8_t)((h << 4) | l);
                    k += 2;
                } else if (c == '+') {
                    c = ' ';
                }
                if (o + 1 >= cap) return -2;
                out[o++] = (char)c;
            }
            out[o] = 0;
            return (int)o;
        }
        /* skip to next pair */
        while (j < qlen && q[j] != '&') j++;
        i = j + 1;
    }
    return -1;
}
