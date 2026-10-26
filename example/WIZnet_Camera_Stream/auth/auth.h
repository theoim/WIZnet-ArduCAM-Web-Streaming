/**
 * @file    auth.h
 * @brief   The two accounts and how a password is checked (Phase 3b).
 *
 * One admin and one viewer (decision Q2, 2026-10-23). There is no default
 * password: until the admin account exists the device is unprovisioned and
 * only setup mode can create it.
 *
 * A password is never stored. What is stored is PBKDF2-HMAC-SHA256 of it with
 * a 16-byte random salt per account, and the iteration count used, so the
 * count can be raised later without invalidating existing accounts.
 *
 * Measured on the RP2350 at 200 MHz (2026-10-26): 72 us per iteration, linear
 * - 5,000 iterations is 360 ms of core 0 per login. That is far below what is
 * recommended for a server (OWASP: 600,000), which a 200 MHz MCU cannot pay
 * per login. The hash lives in the device's own flash, so reading it takes
 * the board; online guessing is rate limited by the session layer. The
 * minimum password length is what carries the rest.
 *
 * Nothing here touches flash, the network or the RNG: the caller supplies
 * the salt and persists the database. Host-tested with the real mbedTLS PBKDF2.
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_USER_MAX      24u
#define AUTH_PASS_MIN      10u
#define AUTH_PASS_MAX      64u
#define AUTH_SALT_LEN      16u
#define AUTH_HASH_LEN      32u
#define AUTH_ITER_DEFAULT  5000u
#define AUTH_ITER_MIN      1000u
#define AUTH_ITER_MAX      200000u

typedef enum { AUTH_NONE = 0, AUTH_VIEWER = 1, AUTH_ADMIN = 2 } auth_role_t;

/* Stored as-is in the settings record: fixed size, no pointers. */
typedef struct {
    uint8_t  present;                   /* 1 when this account has been set */
    uint8_t  enabled;                   /* viewer can be switched off       */
    uint8_t  rsv[2];
    uint32_t iter;
    char     user[32];                  /* NUL-terminated                   */
    uint8_t  salt[AUTH_SALT_LEN];
    uint8_t  hash[AUTH_HASH_LEN];
} auth_account_t;

typedef struct {
    auth_account_t acct[2];             /* [0] admin, [1] viewer */
} auth_db_t;

typedef enum {
    AUTH_OK = 0,
    AUTH_E_USER,        /* empty, too long, or a character outside [A-Za-z0-9._-] */
    AUTH_E_PASS_SHORT,  /* fewer than AUTH_PASS_MIN characters                    */
    AUTH_E_PASS_LONG,   /* more than AUTH_PASS_MAX                                */
    AUTH_E_PASS_CHARS,  /* not printable ASCII                                    */
    AUTH_E_SAME_USER,   /* viewer name equal to the admin's                       */
    AUTH_E_ROLE,        /* not AUTH_ADMIN or AUTH_VIEWER                          */
    AUTH_E_ITER,        /* iteration count outside the limits                     */
    AUTH_E_CRYPTO,      /* PBKDF2 failed                                          */
} auth_err_t;

const char *auth_err_name(auth_err_t e);
const char *auth_role_name(auth_role_t r);

void auth_db_init(auth_db_t *db);
bool auth_db_valid(const auth_db_t *db);       /* sane enough to load from flash */
bool auth_provisioned(const auth_db_t *db);    /* the admin account exists        */

auth_err_t auth_check_user(const char *user, size_t len);
auth_err_t auth_check_password(const char *pass, size_t len);

/** Set (or replace) one account. The salt comes from the caller's RNG. */
auth_err_t auth_set(auth_db_t *db, auth_role_t role, const char *user, size_t ulen,
                    const char *pass, size_t plen, const uint8_t salt[AUTH_SALT_LEN],
                    uint32_t iter);

void auth_viewer_enable(auth_db_t *db, bool on);

/**
 * The role these credentials grant, or AUTH_NONE. A user name that matches no
 * account still costs one PBKDF2, so the time taken does not say whether the
 * name exists. A disabled viewer gets AUTH_NONE.
 */
auth_role_t auth_verify(const auth_db_t *db, const char *user, size_t ulen,
                        const char *pass, size_t plen);

#endif /* AUTH_H */
