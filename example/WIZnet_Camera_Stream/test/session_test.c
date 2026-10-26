/*
 * Host tests for auth/session.c - cookie parsing, expiry, eviction, CSRF,
 * login throttle. Built and run by run_host_tests.sh.
 */

#include <stdio.h>
#include <string.h>

#include "session.h"

static int g_checks, g_fail;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

static void rnd(uint8_t out[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(seed * 37u + (unsigned)i * 11u + 1u);
}

static const sess_t *look(sess_table_t *t, const char *cookie, uint64_t now)
{
    return sess_lookup(t, cookie, strlen(cookie), now, true);
}

static void cookie_for(const sess_t *s, char *out, size_t cap, const char *before, const char *after)
{
    char hex[SESS_HEX_LEN + 1];
    sess_hex(s->token, hex);
    snprintf(out, cap, "%s" SESS_COOKIE "=%s%s", before, hex, after);
}

static void test_create_lookup(void)
{
    sess_table_t t;
    uint8_t r[32];
    char c[160];
    sess_init(&t);
    rnd(r, 1);
    const sess_t *s = sess_create(&t, AUTH_ADMIN, r, 1000);
    CHECK(s && s->used && s->role == AUTH_ADMIN && s->id == 1);
    CHECK(sess_count(&t, 1000) == 1);

    cookie_for(s, c, sizeof(c), "", "");
    CHECK(look(&t, c, 2000) == s);
    cookie_for(s, c, sizeof(c), "theme=dark; ", "; other=1");
    CHECK(look(&t, c, 2000) == s);
    cookie_for(s, c, sizeof(c), "a=1;", "");               /* no space after ';' */
    CHECK(look(&t, c, 2000) == s);
    cookie_for(s, c, sizeof(c), "x" , "");                 /* "xcam_sid=" is another name */
    CHECK(look(&t, c, 2000) == NULL);
    cookie_for(s, c, sizeof(c), "", "0");                  /* 33 hex chars */
    CHECK(look(&t, c, 2000) == NULL);
    CHECK(look(&t, SESS_COOKIE "=", 2000) == NULL);
    CHECK(look(&t, SESS_COOKIE "=00000000000000000000000000000000", 2000) == NULL);  /* empty slots */
    CHECK(look(&t, "", 2000) == NULL);
    CHECK(sess_lookup(&t, NULL, 0, 2000, true) == NULL);

    /* uppercase hex is the same token */
    char hex[SESS_HEX_LEN + 1];
    sess_hex(s->token, hex);
    for (char *p = hex; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
    snprintf(c, sizeof(c), SESS_COOKIE "=%s", hex);
    CHECK(look(&t, c, 2000) == s);

    /* a stale cookie first, the live one second: the live one is found */
    char live[SESS_HEX_LEN + 1];
    sess_hex(s->token, live);
    snprintf(c, sizeof(c), SESS_COOKIE "=ffffffffffffffffffffffffffffffff; " SESS_COOKIE "=%s", live);
    CHECK(look(&t, c, 2000) == s);
}

static void test_expiry(void)
{
    sess_table_t t;
    uint8_t r[32];
    char c[160];
    sess_init(&t);
    rnd(r, 2);
    const sess_t *s = sess_create(&t, AUTH_VIEWER, r, 0);
    cookie_for(s, c, sizeof(c), "", "");
    CHECK(look(&t, c, SESS_IDLE_MS) == s);                 /* exactly idle: still alive, touched */
    CHECK(look(&t, c, 2 * SESS_IDLE_MS) == s);
    CHECK(look(&t, c, 3 * SESS_IDLE_MS + 1) == NULL);      /* idle too long */
    CHECK(sess_count(&t, 3 * SESS_IDLE_MS + 1) == 0);

    /* absolute limit even when used all the time */
    s = sess_create(&t, AUTH_VIEWER, r, 0);
    cookie_for(s, c, sizeof(c), "", "");
    uint64_t now = 0;
    while (now + 60000 <= SESS_ABS_MS) {
        now += 60000;
        CHECK(look(&t, c, now) == s);
    }
    CHECK(look(&t, c, SESS_ABS_MS + 1) == NULL);

    /* not touching does not refresh */
    s = sess_create(&t, AUTH_VIEWER, r, 0);
    cookie_for(s, c, sizeof(c), "", "");
    CHECK(sess_lookup(&t, c, strlen(c), SESS_IDLE_MS - 1, false) == s);
    CHECK(sess_lookup(&t, c, strlen(c), SESS_IDLE_MS + 1, false) == NULL);
}

static void test_eviction_and_end(void)
{
    sess_table_t t;
    uint8_t r[32];
    char c[SESS_MAX + 1][160];
    const sess_t *s[SESS_MAX + 1];
    sess_init(&t);
    for (unsigned i = 0; i < SESS_MAX; i++) {
        rnd(r, (uint8_t)(10 + i));
        s[i] = sess_create(&t, i == 0 ? AUTH_ADMIN : AUTH_VIEWER, r, 100 * (i + 1));
        cookie_for(s[i], c[i], sizeof(c[i]), "", "");
    }
    CHECK(sess_count(&t, 1000) == SESS_MAX);
    CHECK(look(&t, c[0], 1000) == s[0]);                   /* 0 is now the most recent */
    uint32_t id1 = s[1]->id;
    rnd(r, 99);
    s[SESS_MAX] = sess_create(&t, AUTH_VIEWER, r, 1100);   /* evicts 1, the oldest */
    CHECK(s[SESS_MAX]->id != id1 && s[SESS_MAX]->id == SESS_MAX + 1);
    CHECK(look(&t, c[1], 1200) == NULL);
    CHECK(look(&t, c[0], 1200) != NULL);

    CHECK(sess_alive(&t, s[0]->id, 1300));
    CHECK(!sess_alive(&t, id1, 1300));
    CHECK(!sess_alive(&t, s[0]->id, 1200 + SESS_IDLE_MS + 1));
    sess_end(&t, s[0]->id);
    CHECK(look(&t, c[0], 1300) == NULL);
    CHECK(!sess_alive(&t, s[0]->id, 1300));
    sess_end_role(&t, AUTH_VIEWER);
    CHECK(sess_count(&t, 1300) == 0);
}

static void test_csrf(void)
{
    sess_table_t t;
    uint8_t r[32];
    char hex[SESS_HEX_LEN + 1];
    sess_init(&t);
    rnd(r, 5);
    const sess_t *s = sess_create(&t, AUTH_ADMIN, r, 0);
    sess_hex(s->csrf, hex);
    CHECK(sess_csrf_ok(s, hex, strlen(hex)));
    char tokhex[SESS_HEX_LEN + 1];
    sess_hex(s->token, tokhex);
    CHECK(!sess_csrf_ok(s, tokhex, strlen(tokhex)));       /* the cookie is not the CSRF token */
    hex[5] ^= 1;
    CHECK(!sess_csrf_ok(s, hex, strlen(hex)));
    CHECK(!sess_csrf_ok(s, "", 0));
    CHECK(!sess_csrf_ok(s, NULL, 0));
    CHECK(!sess_csrf_ok(NULL, hex, strlen(hex)));
    CHECK(memcmp(s->token, s->csrf, SESS_TOKEN_LEN) != 0);
}

static void test_throttle(void)
{
    sess_table_t t;
    uint32_t retry = 1;
    sess_init(&t);
    CHECK(sess_login_allowed(&t, 0, &retry) && retry == 0);
    for (unsigned i = 0; i < SESS_FAIL_MAX - 1; i++) sess_login_result(&t, false, 1000);
    CHECK(sess_login_allowed(&t, 1000, &retry));
    sess_login_result(&t, true, 1000);                     /* success resets the count */
    for (unsigned i = 0; i < SESS_FAIL_MAX - 1; i++) sess_login_result(&t, false, 2000);
    CHECK(sess_login_allowed(&t, 2000, &retry));
    sess_login_result(&t, false, 2000);                    /* the fifth */
    CHECK(!sess_login_allowed(&t, 2000, &retry) && retry == SESS_LOCK_MS);
    CHECK(!sess_login_allowed(&t, 2000 + SESS_LOCK_MS - 1, &retry) && retry == 1);
    CHECK(sess_login_allowed(&t, 2000 + SESS_LOCK_MS, &retry));
    /* after the lock a fresh run of five is needed again */
    for (unsigned i = 0; i < SESS_FAIL_MAX - 1; i++) sess_login_result(&t, false, 40000);
    CHECK(sess_login_allowed(&t, 40000, &retry));
}

static void test_ids_unique(void)
{
    sess_table_t t;
    uint8_t r[32];
    sess_init(&t);
    uint32_t last = 0;
    for (int i = 0; i < 50; i++) {
        rnd(r, (uint8_t)i);
        const sess_t *s = sess_create(&t, AUTH_VIEWER, r, (uint64_t)i);
        CHECK(s->id > last);
        last = s->id;
    }
}

static void test_policy(void)
{
    sess_t admin = { .used = true, .role = AUTH_ADMIN, .id = 1 };
    sess_t viewer = { .used = true, .role = AUTH_VIEWER, .id = 2 };
    const char *why = NULL;
    /* public */
    CHECK(sess_policy(SESS_NEED_NONE, false, NULL, true, false, &why) == 0);
    /* unprovisioned: nothing but public */
    CHECK(sess_policy(SESS_NEED_VIEWER, false, &admin, false, true, &why) == 403 &&
          strcmp(why, "setup required") == 0);
    /* no session */
    CHECK(sess_policy(SESS_NEED_VIEWER, true, NULL, false, false, &why) == 401);
    CHECK(sess_policy(SESS_NEED_ADMIN, true, NULL, true, true, &why) == 401);
    /* roles */
    CHECK(sess_policy(SESS_NEED_VIEWER, true, &viewer, false, false, &why) == 0);
    CHECK(sess_policy(SESS_NEED_VIEWER, true, &admin, false, false, &why) == 0);
    CHECK(sess_policy(SESS_NEED_ADMIN, true, &viewer, false, false, &why) == 403 &&
          strcmp(why, "admin only") == 0);
    CHECK(sess_policy(SESS_NEED_ADMIN, true, &viewer, true, true, &why) == 403);
    CHECK(sess_policy(SESS_NEED_ADMIN, true, &admin, false, false, &why) == 0);
    /* CSRF on POST only */
    CHECK(sess_policy(SESS_NEED_ADMIN, true, &admin, true, false, &why) == 403 &&
          strstr(why, "CSRF") != NULL);
    CHECK(sess_policy(SESS_NEED_ADMIN, true, &admin, true, true, &why) == 0);
    CHECK(sess_policy(SESS_NEED_VIEWER, true, &viewer, true, false, &why) == 403);
    CHECK(sess_policy(SESS_NEED_VIEWER, true, &viewer, true, true, &why) == 0);
    sess_t odd = { .used = true, .role = AUTH_NONE, .id = 3 };
    CHECK(sess_policy(SESS_NEED_VIEWER, true, &odd, false, false, &why) == 403);
    CHECK(sess_policy(SESS_NEED_VIEWER, true, NULL, false, false, NULL) == 401);  /* why optional */

    /* stream */
    CHECK(sess_stream_policy(0, AUTH_NONE, 5, AUTH_VIEWER) == SESS_STREAM_TAKE);
    CHECK(sess_stream_policy(5, AUTH_VIEWER, 5, AUTH_VIEWER) == SESS_STREAM_REPLACE);   /* own reconnect */
    CHECK(sess_stream_policy(5, AUTH_ADMIN, 5, AUTH_ADMIN) == SESS_STREAM_REPLACE);
    CHECK(sess_stream_policy(5, AUTH_VIEWER, 6, AUTH_ADMIN) == SESS_STREAM_REPLACE);    /* admin over viewer */
    CHECK(sess_stream_policy(5, AUTH_ADMIN, 6, AUTH_VIEWER) == SESS_STREAM_REFUSE);     /* never viewer over admin */
    CHECK(sess_stream_policy(5, AUTH_VIEWER, 6, AUTH_VIEWER) == SESS_STREAM_REFUSE);    /* same role, other session */
    CHECK(sess_stream_policy(5, AUTH_ADMIN, 6, AUTH_ADMIN) == SESS_STREAM_REFUSE);
}

int main(void)
{
    test_policy();
    test_create_lookup();
    test_expiry();
    test_eviction_and_end();
    test_csrf();
    test_throttle();
    test_ids_unique();
    printf("session_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
