/**
 * @file    session.c
 * @brief   Login sessions and throttle - see session.h.
 */

#include <string.h>

#include "session.h"

void sess_init(sess_table_t *t)
{
    memset(t, 0, sizeof(*t));
    t->next_id = 1;
}

static bool expired(const sess_t *s, uint64_t now)
{
    return now - s->last_ms > SESS_IDLE_MS || now - s->created_ms > SESS_ABS_MS;
}

static void wipe(sess_t *s)
{
    volatile uint8_t *p = (volatile uint8_t *)s;
    for (size_t i = 0; i < sizeof(*s); i++) p[i] = 0;
}

const sess_t *sess_create(sess_table_t *t, auth_role_t role,
                          const uint8_t rnd[2 * SESS_TOKEN_LEN], uint64_t now_ms)
{
    int slot = -1;
    for (unsigned i = 0; i < SESS_MAX; i++) {
        if (t->s[i].used && expired(&t->s[i], now_ms)) wipe(&t->s[i]);
        if (!t->s[i].used && slot < 0) slot = (int)i;
    }
    if (slot < 0) {                     /* all live: the least recently used goes */
        slot = 0;
        for (unsigned i = 1; i < SESS_MAX; i++) {
            if (t->s[i].last_ms < t->s[slot].last_ms) slot = (int)i;
        }
    }
    sess_t *s = &t->s[slot];
    wipe(s);
    s->used       = true;
    s->role       = role;
    s->id         = t->next_id++;
    if (t->next_id == 0) t->next_id = 1;
    memcpy(s->token, rnd, SESS_TOKEN_LEN);
    memcpy(s->csrf, rnd + SESS_TOKEN_LEN, SESS_TOKEN_LEN);
    s->created_ms = now_ms;
    s->last_ms    = now_ms;
    return s;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool unhex(const char *h, size_t len, uint8_t out[SESS_TOKEN_LEN])
{
    if (len != SESS_HEX_LEN) return false;
    for (size_t i = 0; i < SESS_TOKEN_LEN; i++) {
        int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}

static bool ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* Find the session whose token is @p tok; every slot is compared. */
static sess_t *by_token(sess_table_t *t, const uint8_t tok[SESS_TOKEN_LEN])
{
    sess_t *hit = NULL;
    for (unsigned i = 0; i < SESS_MAX; i++) {
        bool eq = ct_eq(t->s[i].token, tok, SESS_TOKEN_LEN);
        if (t->s[i].used && eq && !hit) hit = &t->s[i];
    }
    return hit;
}

const sess_t *sess_lookup(sess_table_t *t, const char *cookie, size_t len,
                          uint64_t now_ms, bool touch)
{
    static const char name[] = SESS_COOKIE "=";
    const size_t nlen = sizeof(name) - 1;
    size_t i = 0;

    if (!cookie) return NULL;
    while (i < len) {
        while (i < len && (cookie[i] == ' ' || cookie[i] == '\t')) i++;
        size_t start = i;
        while (i < len && cookie[i] != ';') i++;
        size_t end = i;                 /* [start, end) is one name=value pair */
        if (i < len) i++;               /* past ';' */
        while (end > start && (cookie[end - 1] == ' ' || cookie[end - 1] == '\t')) end--;
        if (end - start < nlen || memcmp(cookie + start, name, nlen) != 0) continue;

        uint8_t tok[SESS_TOKEN_LEN];
        if (!unhex(cookie + start + nlen, end - start - nlen, tok)) continue;
        sess_t *s = by_token(t, tok);
        if (!s) continue;
        if (expired(s, now_ms)) {
            wipe(s);
            continue;
        }
        if (touch) s->last_ms = now_ms;
        return s;
    }
    return NULL;
}

bool sess_csrf_ok(const sess_t *s, const char *hex, size_t len)
{
    uint8_t tok[SESS_TOKEN_LEN];
    if (!s || !hex || !unhex(hex, len, tok)) return false;
    return ct_eq(s->csrf, tok, SESS_TOKEN_LEN);
}

void sess_end(sess_table_t *t, uint32_t id)
{
    for (unsigned i = 0; i < SESS_MAX; i++) {
        if (t->s[i].used && t->s[i].id == id) wipe(&t->s[i]);
    }
}

void sess_end_role(sess_table_t *t, auth_role_t role)
{
    for (unsigned i = 0; i < SESS_MAX; i++) {
        if (t->s[i].used && t->s[i].role == role) wipe(&t->s[i]);
    }
}

unsigned sess_count(const sess_table_t *t, uint64_t now_ms)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SESS_MAX; i++) {
        if (t->s[i].used && !expired(&t->s[i], now_ms)) n++;
    }
    return n;
}

bool sess_alive(const sess_table_t *t, uint32_t id, uint64_t now_ms)
{
    for (unsigned i = 0; i < SESS_MAX; i++) {
        if (t->s[i].used && t->s[i].id == id) return !expired(&t->s[i], now_ms);
    }
    return false;
}

bool sess_login_allowed(const sess_table_t *t, uint64_t now_ms, uint32_t *retry_ms)
{
    if (now_ms < t->locked_until_ms) {
        if (retry_ms) *retry_ms = (uint32_t)(t->locked_until_ms - now_ms);
        return false;
    }
    if (retry_ms) *retry_ms = 0;
    return true;
}

void sess_login_result(sess_table_t *t, bool ok, uint64_t now_ms)
{
    if (ok) {
        t->fails = 0;
        return;
    }
    if (++t->fails >= SESS_FAIL_MAX) {
        t->fails = 0;
        t->locked_until_ms = now_ms + SESS_LOCK_MS;
    }
}

void sess_hex(const uint8_t in[SESS_TOKEN_LEN], char out[SESS_HEX_LEN + 1])
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < SESS_TOKEN_LEN; i++) {
        out[2 * i]     = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[SESS_HEX_LEN] = 0;
}

int sess_policy(sess_need_t need, bool provisioned, const sess_t *s, bool is_post,
                bool csrf_ok, const char **why)
{
    const char *w = "ok";
    int rc = 0;
    if (need == SESS_NEED_NONE) {
        rc = 0;
    } else if (!provisioned) {
        rc = 403; w = "setup required";
    } else if (!s) {
        rc = 401; w = "login required";
    } else if (need == SESS_NEED_ADMIN && s->role != AUTH_ADMIN) {
        rc = 403; w = "admin only";
    } else if (s->role != AUTH_ADMIN && s->role != AUTH_VIEWER) {
        rc = 403; w = "no role";
    } else if (is_post && !csrf_ok) {
        rc = 403; w = "missing or wrong X-CSRF-Token";
    }
    if (why) *why = w;
    return rc;
}

sess_stream_t sess_stream_policy(uint32_t owner_id, auth_role_t owner_role,
                                 uint32_t new_id, auth_role_t new_role)
{
    if (owner_id == 0) return SESS_STREAM_TAKE;
    if (owner_id == new_id) return SESS_STREAM_REPLACE;
    if (new_role == AUTH_ADMIN && owner_role == AUTH_VIEWER) return SESS_STREAM_REPLACE;
    return SESS_STREAM_REFUSE;
}
