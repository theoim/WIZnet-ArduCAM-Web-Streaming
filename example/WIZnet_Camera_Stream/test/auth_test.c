/*
 * Host tests for auth/auth.c, against the real mbedTLS PBKDF2 (the same
 * library sources the board builds). Built and run by run_host_tests.sh.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mbedtls/pkcs5.h"
#include "auth.h"

static int g_checks, g_fail;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

#define S(x) (x), strlen(x)

static const uint8_t k_salt_a[AUTH_SALT_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint8_t k_salt_b[AUTH_SALT_LEN] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };

static void hex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* RFC 7914 section 11 / common PBKDF2-HMAC-SHA256 vectors. */
static void test_vectors(void)
{
    struct { uint32_t c; const char *dk; } v[] = {
        { 1,    "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b" },
        { 2,    "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43" },
        { 4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a" },
    };
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint8_t out[32], want[32];
        hex(v[i].dk, want, 32);
        CHECK(mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)"password", 8,
                                            (const unsigned char *)"salt", 4, v[i].c, 32, out) == 0);
        CHECK(memcmp(out, want, 32) == 0);
    }
}

static void test_policy(void)
{
    CHECK(auth_check_user(S("admin")) == AUTH_OK);
    CHECK(auth_check_user(S("a.b_c-9")) == AUTH_OK);
    CHECK(auth_check_user(S("")) == AUTH_E_USER);
    CHECK(auth_check_user(S("has space")) == AUTH_E_USER);
    CHECK(auth_check_user(S("semi;colon")) == AUTH_E_USER);
    CHECK(auth_check_user(S("\"quote")) == AUTH_E_USER);
    CHECK(auth_check_user(S("abcdefghijklmnopqrstuvwx")) == AUTH_OK);     /* 24 */
    CHECK(auth_check_user(S("abcdefghijklmnopqrstuvwxy")) == AUTH_E_USER); /* 25 */
    CHECK(auth_check_user("adm\0in", 6) == AUTH_E_USER);

    CHECK(auth_check_password(S("123456789")) == AUTH_E_PASS_SHORT);
    CHECK(auth_check_password(S("abcdefghij")) == AUTH_OK);
    CHECK(auth_check_password(S("with space ok!~")) == AUTH_OK);
    char p65[66];
    memset(p65, 'x', 65); p65[65] = 0;
    CHECK(auth_check_password(p65, 64) == AUTH_OK);
    CHECK(auth_check_password(p65, 65) == AUTH_E_PASS_LONG);
    CHECK(auth_check_password("abcdefghi\n", 10) == AUTH_E_PASS_CHARS);
    CHECK(auth_check_password("abcdefghi\x80", 10) == AUTH_E_PASS_CHARS);
    CHECK(auth_check_password("abcde\0ghij", 10) == AUTH_E_PASS_CHARS);
}

static void test_set_verify(void)
{
    auth_db_t db;
    auth_db_init(&db);
    CHECK(!auth_provisioned(&db));
    CHECK(auth_db_valid(&db));
    CHECK(auth_verify(&db, S("admin"), S("correct horse")) == AUTH_NONE);

    CHECK(auth_set(&db, AUTH_ADMIN, S("admin"), S("correct horse"), k_salt_a, AUTH_ITER_MIN) == AUTH_OK);
    CHECK(auth_provisioned(&db) && auth_db_valid(&db));
    CHECK(auth_verify(&db, S("admin"), S("correct horse")) == AUTH_ADMIN);
    CHECK(auth_verify(&db, S("admin"), S("correct hors")) == AUTH_NONE);
    CHECK(auth_verify(&db, S("admin"), S("correct horsf")) == AUTH_NONE);
    CHECK(auth_verify(&db, S("Admin"), S("correct horse")) == AUTH_NONE);   /* names are exact */
    CHECK(auth_verify(&db, S("admi"), S("correct horse")) == AUTH_NONE);
    CHECK(auth_verify(&db, S("adminx"), S("correct horse")) == AUTH_NONE);
    CHECK(auth_verify(&db, S(""), S("")) == AUTH_NONE);

    /* the password is not stored */
    {
        const uint8_t *raw = (const uint8_t *)&db;
        int found = 0;
        for (size_t i = 0; i + 13 <= sizeof(db); i++) {
            if (memcmp(raw + i, "correct horse", 13) == 0) found = 1;
        }
        CHECK(!found);
    }

    /* viewer */
    CHECK(auth_set(&db, AUTH_VIEWER, S("admin"), S("viewer pass 1"), k_salt_b, AUTH_ITER_MIN) == AUTH_E_SAME_USER);
    CHECK(auth_set(&db, AUTH_VIEWER, S("guest"), S("short"), k_salt_b, AUTH_ITER_MIN) == AUTH_E_PASS_SHORT);
    CHECK(auth_set(&db, AUTH_VIEWER, S("guest"), S("viewer pass 1"), k_salt_b, 10) == AUTH_E_ITER);
    CHECK(auth_set(&db, AUTH_NONE, S("guest"), S("viewer pass 1"), k_salt_b, AUTH_ITER_MIN) == AUTH_E_ROLE);
    CHECK(db.acct[1].present == 0);                     /* refusals change nothing */
    CHECK(auth_set(&db, AUTH_VIEWER, S("guest"), S("viewer pass 1"), k_salt_b, AUTH_ITER_MIN) == AUTH_OK);
    CHECK(auth_verify(&db, S("guest"), S("viewer pass 1")) == AUTH_VIEWER);
    CHECK(auth_verify(&db, S("guest"), S("correct horse")) == AUTH_NONE);   /* no cross-over */
    CHECK(auth_verify(&db, S("admin"), S("viewer pass 1")) == AUTH_NONE);
    auth_viewer_enable(&db, false);
    CHECK(auth_verify(&db, S("guest"), S("viewer pass 1")) == AUTH_NONE);
    CHECK(auth_verify(&db, S("admin"), S("correct horse")) == AUTH_ADMIN);
    auth_viewer_enable(&db, true);
    CHECK(auth_verify(&db, S("guest"), S("viewer pass 1")) == AUTH_VIEWER);

    /* same password, different salt -> different hash */
    auth_db_t d2;
    auth_db_init(&d2);
    CHECK(auth_set(&d2, AUTH_ADMIN, S("admin"), S("correct horse"), k_salt_b, AUTH_ITER_MIN) == AUTH_OK);
    CHECK(memcmp(d2.acct[0].hash, db.acct[0].hash, AUTH_HASH_LEN) != 0);

    /* password change replaces the old one */
    CHECK(auth_set(&db, AUTH_ADMIN, S("admin"), S("new password!"), k_salt_b, AUTH_ITER_MIN) == AUTH_OK);
    CHECK(auth_verify(&db, S("admin"), S("correct horse")) == AUTH_NONE);
    CHECK(auth_verify(&db, S("admin"), S("new password!")) == AUTH_ADMIN);
}

/* What comes back from flash is checked before it is trusted. */
static void test_db_valid(void)
{
    auth_db_t db;
    auth_db_init(&db);
    CHECK(auth_set(&db, AUTH_ADMIN, S("admin"), S("correct horse"), k_salt_a, AUTH_ITER_MIN) == AUTH_OK);
    auth_db_t bad = db;
    bad.acct[0].present = 0xFF;                 /* erased flash */
    CHECK(!auth_db_valid(&bad));
    bad = db; bad.acct[0].iter = 0xFFFFFFFFu;
    CHECK(!auth_db_valid(&bad));
    bad = db; memset(bad.acct[0].user, 'a', sizeof(bad.acct[0].user));   /* no NUL */
    CHECK(!auth_db_valid(&bad));
    bad = db; bad.acct[0].user[0] = ' ';
    CHECK(!auth_db_valid(&bad));
    bad = db; bad.acct[0].enabled = 7;
    CHECK(!auth_db_valid(&bad));
}

/* An unknown user name costs about as much as a wrong password. */
static void test_timing(void)
{
    auth_db_t db;
    auth_db_init(&db);
    CHECK(auth_set(&db, AUTH_ADMIN, S("admin"), S("correct horse"), k_salt_a, AUTH_ITER_DEFAULT) == AUTH_OK);
    clock_t t0 = clock();
    for (int i = 0; i < 3; i++) (void)auth_verify(&db, S("admin"), S("wrong password"));
    clock_t t1 = clock();
    for (int i = 0; i < 3; i++) (void)auth_verify(&db, S("nobody"), S("wrong password"));
    clock_t t2 = clock();
    double known = (double)(t1 - t0), unknown = (double)(t2 - t1);
    printf("verify x3: known user %.0f ticks, unknown user %.0f ticks\n", known, unknown);
    CHECK(unknown > known * 0.5 && unknown < known * 2.0);
}

int main(void)
{
    test_vectors();
    test_policy();
    test_set_verify();
    test_db_valid();
    test_timing();
    printf("auth_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
