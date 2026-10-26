/**
 * @file    session.h
 * @brief   Login sessions and the login throttle (Phase 3b).
 *
 * A session is a random 128-bit token, sent to the browser as a cookie
 * (Secure, HttpOnly, SameSite=Strict) and kept here with its role. A second
 * random value, the CSRF token, is given to the page in the login reply and
 * must come back in an X-CSRF-Token header on every POST: the cookie alone
 * proves the browser, not that this page sent the request.
 *
 * Sessions live in RAM only: a reboot logs everyone out. They end on logout,
 * after SESS_IDLE_MS without a request, SESS_ABS_MS after login, when the
 * account's password changes, or when a new login needs the slot (the least
 * recently used one goes).
 *
 * Throttle: SESS_FAIL_MAX wrong passwords in a row lock login for
 * SESS_LOCK_MS. One counter for the device - with two accounts there is no
 * per-user state worth keeping, and a per-address one would need the peer IP.
 *
 * Time is passed in (ms since boot, 64-bit) and randomness is passed in, so
 * this compiles and is tested on the host.
 */

#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth.h"

#define SESS_MAX        4u
#define SESS_TOKEN_LEN  16u                       /* bytes; 32 hex characters  */
#define SESS_HEX_LEN    (2u * SESS_TOKEN_LEN)
#define SESS_IDLE_MS    (30ull * 60u * 1000u)
#define SESS_ABS_MS     (12ull * 3600u * 1000u)
#define SESS_FAIL_MAX   5u
#define SESS_LOCK_MS    30000u
#define SESS_COOKIE     "cam_sid"

typedef struct {
    bool        used;
    auth_role_t role;
    uint32_t    id;                     /* never reused: the stream owner tag */
    uint8_t     token[SESS_TOKEN_LEN];
    uint8_t     csrf[SESS_TOKEN_LEN];
    uint64_t    created_ms, last_ms;
} sess_t;

typedef struct {
    sess_t   s[SESS_MAX];
    uint32_t next_id;
    uint32_t fails;                     /* consecutive wrong passwords */
    uint64_t locked_until_ms;
} sess_table_t;

void sess_init(sess_table_t *t);

/**
 * New session for @p role from 32 random bytes (token, then CSRF token).
 * Evicts the least recently used session when all slots are taken.
 */
const sess_t *sess_create(sess_table_t *t, auth_role_t role,
                          const uint8_t rnd[2 * SESS_TOKEN_LEN], uint64_t now_ms);

/**
 * The live session named by @p cookie_header (the whole Cookie header value,
 * "a=1; cam_sid=<hex>; b=2"), or NULL. Expired sessions are dropped on the
 * way. A hit refreshes the idle timer when @p touch is set.
 */
const sess_t *sess_lookup(sess_table_t *t, const char *cookie_header, size_t len,
                          uint64_t now_ms, bool touch);

/** Constant-time comparison of a hex CSRF token against the session's. */
bool sess_csrf_ok(const sess_t *s, const char *hex, size_t len);

void sess_end(sess_table_t *t, uint32_t id);
void sess_end_role(sess_table_t *t, auth_role_t role);
unsigned sess_count(const sess_table_t *t, uint64_t now_ms);
bool sess_alive(const sess_table_t *t, uint32_t id, uint64_t now_ms);

/* Throttle. allowed() says whether a login may be tried now; *retry_ms is
 * how long until it may. result() records the outcome of a tried login. */
bool sess_login_allowed(const sess_table_t *t, uint64_t now_ms, uint32_t *retry_ms);
void sess_login_result(sess_table_t *t, bool ok, uint64_t now_ms);

/* ------------------------------ Policies ---------------------------------
 * Pure decisions, kept here so the host tests cover them. */

typedef enum {
    SESS_NEED_NONE = 0,     /* page shell, login, setup: anyone            */
    SESS_NEED_VIEWER,       /* watching                                     */
    SESS_NEED_ADMIN,        /* changing anything; diagnostics               */
} sess_need_t;

/**
 * 0 to serve the request, else the status to answer: 401 (no session),
 * 403 (wrong role, missing/bad CSRF token on a POST, or the device has no
 * admin yet). @p why gets a short reason for the JSON body.
 */
int sess_policy(sess_need_t need, bool provisioned, const sess_t *s, bool is_post,
                bool csrf_ok, const char **why);

typedef enum {
    SESS_STREAM_TAKE = 0,   /* nobody streaming                             */
    SESS_STREAM_REPLACE,    /* close the current stream, give it to the new  */
    SESS_STREAM_REFUSE,     /* keep the current one, refuse the new          */
} sess_stream_t;

/**
 * One stream at a time (decision Q4, with the 2026-10-26 amendment):
 * the same session reconnecting replaces its own stream; an admin replaces a
 * viewer; a viewer never replaces an admin; two different sessions of the
 * same role - the newcomer is refused. @p owner_id 0 means no stream.
 */
sess_stream_t sess_stream_policy(uint32_t owner_id, auth_role_t owner_role,
                                 uint32_t new_id, auth_role_t new_role);

/* 32 lowercase hex characters + NUL. */
void sess_hex(const uint8_t in[SESS_TOKEN_LEN], char out[SESS_HEX_LEN + 1]);

#endif /* SESSION_H */
