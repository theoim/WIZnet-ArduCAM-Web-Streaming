/**
 * @file    auth.c
 * @brief   Accounts and password checks - see auth.h.
 */

#include <string.h>

#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"

#include "auth.h"

/* bounded_len() is POSIX, not C99. */
static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

const char *auth_err_name(auth_err_t e)
{
    switch (e) {
    case AUTH_OK:           return "ok";
    case AUTH_E_USER:       return "user name: 1-24 of A-Z a-z 0-9 . _ -";
    case AUTH_E_PASS_SHORT: return "password too short (at least 10)";
    case AUTH_E_PASS_LONG:  return "password too long (at most 64)";
    case AUTH_E_PASS_CHARS: return "password: printable ASCII only";
    case AUTH_E_SAME_USER:  return "viewer name must differ from the admin's";
    case AUTH_E_ROLE:       return "no such role";
    case AUTH_E_ITER:       return "iteration count out of range";
    case AUTH_E_CRYPTO:     return "hash failed";
    }
    return "?";
}

const char *auth_role_name(auth_role_t r)
{
    switch (r) {
    case AUTH_ADMIN:  return "admin";
    case AUTH_VIEWER: return "viewer";
    case AUTH_NONE:   break;
    }
    return "none";
}

void auth_db_init(auth_db_t *db)
{
    memset(db, 0, sizeof(*db));
}

static bool account_valid(const auth_account_t *a)
{
    if (!a->present) {
        return true;                    /* an empty slot is fine */
    }
    if (a->present != 1 || a->enabled > 1) return false;
    if (a->iter < AUTH_ITER_MIN || a->iter > AUTH_ITER_MAX) return false;
    size_t n = bounded_len(a->user, sizeof(a->user));
    return n < sizeof(a->user) && auth_check_user(a->user, n) == AUTH_OK;
}

bool auth_db_valid(const auth_db_t *db)
{
    return account_valid(&db->acct[0]) && account_valid(&db->acct[1]);
}

bool auth_provisioned(const auth_db_t *db)
{
    return db->acct[0].present == 1;
}

auth_err_t auth_check_user(const char *user, size_t len)
{
    if (len == 0 || len > AUTH_USER_MAX) return AUTH_E_USER;
    for (size_t i = 0; i < len; i++) {
        char c = user[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return AUTH_E_USER;
    }
    return AUTH_OK;
}

auth_err_t auth_check_password(const char *pass, size_t len)
{
    if (len < AUTH_PASS_MIN) return AUTH_E_PASS_SHORT;
    if (len > AUTH_PASS_MAX) return AUTH_E_PASS_LONG;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pass[i];
        if (c < 0x20 || c > 0x7E) return AUTH_E_PASS_CHARS;
    }
    return AUTH_OK;
}

static int kdf(const char *pass, size_t plen, const uint8_t *salt, uint32_t iter,
               uint8_t out[AUTH_HASH_LEN])
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pass, plen,
                                         salt, AUTH_SALT_LEN, iter, AUTH_HASH_LEN, out);
}

static bool name_eq(const auth_account_t *a, const char *user, size_t ulen)
{
    return a->present == 1 && bounded_len(a->user, sizeof(a->user)) == ulen &&
           memcmp(a->user, user, ulen) == 0;
}

auth_err_t auth_set(auth_db_t *db, auth_role_t role, const char *user, size_t ulen,
                    const char *pass, size_t plen, const uint8_t salt[AUTH_SALT_LEN],
                    uint32_t iter)
{
    if (role != AUTH_ADMIN && role != AUTH_VIEWER) return AUTH_E_ROLE;
    auth_err_t e = auth_check_user(user, ulen);
    if (e != AUTH_OK) return e;
    e = auth_check_password(pass, plen);
    if (e != AUTH_OK) return e;
    if (iter < AUTH_ITER_MIN || iter > AUTH_ITER_MAX) return AUTH_E_ITER;
    const auth_account_t *other = &db->acct[role == AUTH_ADMIN ? 1 : 0];
    if (name_eq(other, user, ulen)) return AUTH_E_SAME_USER;

    auth_account_t a;
    memset(&a, 0, sizeof(a));
    a.present = 1;
    a.enabled = 1;
    a.iter    = iter;
    memcpy(a.user, user, ulen);
    memcpy(a.salt, salt, AUTH_SALT_LEN);
    if (kdf(pass, plen, a.salt, iter, a.hash) != 0) {
        mbedtls_platform_zeroize(&a, sizeof(a));
        return AUTH_E_CRYPTO;
    }
    db->acct[role == AUTH_ADMIN ? 0 : 1] = a;
    mbedtls_platform_zeroize(&a, sizeof(a));
    return AUTH_OK;
}

void auth_viewer_enable(auth_db_t *db, bool on)
{
    if (db->acct[1].present == 1) {
        db->acct[1].enabled = on ? 1 : 0;
    }
}

auth_role_t auth_verify(const auth_db_t *db, const char *user, size_t ulen,
                        const char *pass, size_t plen)
{
    static const uint8_t dummy_salt[AUTH_SALT_LEN] = { 0 };
    const auth_account_t *a = NULL;
    auth_role_t role = AUTH_NONE;

    if (name_eq(&db->acct[0], user, ulen)) {
        a = &db->acct[0];
        role = AUTH_ADMIN;
    } else if (name_eq(&db->acct[1], user, ulen)) {
        a = &db->acct[1];
        role = AUTH_VIEWER;
    }

    uint8_t h[AUTH_HASH_LEN];
    if (!a || auth_check_password(pass, plen) != AUTH_OK) {
        /* Same cost as a real check: an unknown name or a password that could
         * never have been set is not answered faster. */
        (void)kdf(pass, plen > AUTH_PASS_MAX ? AUTH_PASS_MAX : plen, dummy_salt,
                  AUTH_ITER_DEFAULT, h);
        mbedtls_platform_zeroize(h, sizeof(h));
        return AUTH_NONE;
    }
    if (kdf(pass, plen, a->salt, a->iter, h) != 0) {
        mbedtls_platform_zeroize(h, sizeof(h));
        return AUTH_NONE;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < AUTH_HASH_LEN; i++) {
        diff |= (uint8_t)(h[i] ^ a->hash[i]);
    }
    mbedtls_platform_zeroize(h, sizeof(h));
    if (diff != 0) return AUTH_NONE;
    if (role == AUTH_VIEWER && !a->enabled) return AUTH_NONE;
    return role;
}
