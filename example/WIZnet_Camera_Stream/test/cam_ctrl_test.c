/*
 * Host tests for cam/cam_ctrl.c - control lookup, strict values, batches,
 * written values. Built and run by run_host_tests.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cam_ctrl.h"

static int g_checks, g_fail;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

static const char *const k_awb[] = { "auto", "sunny", "office", "cloudy", "home" };

static const cam_ctrl_desc_t k_table[] = {
    { "brightness", "Brightness", CAM_CTRL_INT,  CAM_CTRL_F_PROVISIONAL, -4, 4, NULL,  0x0122 },
    { "flip",       "Flip",       CAM_CTRL_BOOL, 0,                       0, 1, NULL,  0x012B },
    { "awb",        "White bal.", CAM_CTRL_ENUM, 0,                       0, 4, k_awb, 0x0126 },
    { "quality",    "Quality",    CAM_CTRL_INT,  0,                       0, 255, NULL, 0x012A },
};
#define N_TABLE ((unsigned)(sizeof(k_table) / sizeof(k_table[0])))

static cam_ctrl_err_t pv(unsigned idx, const char *s, int16_t *out)
{
    return cam_ctrl_parse_value(&k_table[idx], s, strlen(s), out);
}

static void test_find(void)
{
    CHECK(cam_ctrl_find(k_table, N_TABLE, "brightness", 10) == 0);
    CHECK(cam_ctrl_find(k_table, N_TABLE, "quality", 7) == 3);
    CHECK(cam_ctrl_find(k_table, N_TABLE, "Brightness", 10) == -1);   /* case */
    CHECK(cam_ctrl_find(k_table, N_TABLE, "bright", 6) == -1);        /* prefix */
    CHECK(cam_ctrl_find(k_table, N_TABLE, "brightnessX", 11) == -1);
    CHECK(cam_ctrl_find(k_table, N_TABLE, "", 0) == -1);
    CHECK(cam_ctrl_find(NULL, 0, "flip", 4) == -1);
    CHECK(cam_ctrl_find(k_table, 0, "flip", 4) == -1);                /* empty table */
}

static void test_values(void)
{
    int16_t v = 99;

    /* in range, both ends */
    CHECK(pv(0, "-4", &v) == CAM_CTRL_OK && v == -4);
    CHECK(pv(0, "4", &v) == CAM_CTRL_OK && v == 4);
    CHECK(pv(0, "0", &v) == CAM_CTRL_OK && v == 0);
    CHECK(pv(3, "255", &v) == CAM_CTRL_OK && v == 255);

    /* out of range: refused, never clamped, *out untouched */
    v = 77;
    CHECK(pv(0, "5", &v) == CAM_CTRL_E_RANGE && v == 77);
    CHECK(pv(0, "-5", &v) == CAM_CTRL_E_RANGE && v == 77);
    CHECK(pv(3, "256", &v) == CAM_CTRL_E_RANGE && v == 77);
    CHECK(pv(3, "99999", &v) == CAM_CTRL_E_RANGE && v == 77);
    CHECK(pv(1, "2", &v) == CAM_CTRL_E_RANGE && v == 77);

    /* syntax */
    const char *bad[] = { "", "-", "+1", " 1", "1 ", "1a", "a", "007", "00", "-0",
                          "123456", "1.0", "0x10", "--1", "1-" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        v = 77;
        CHECK(pv(0, bad[i], &v) == CAM_CTRL_E_SYNTAX && v == 77);
    }

    /* bool takes digits only */
    CHECK(pv(1, "1", &v) == CAM_CTRL_OK && v == 1);
    CHECK(pv(1, "true", &v) == CAM_CTRL_E_SYNTAX);

    /* enum: index or exact label */
    CHECK(pv(2, "office", &v) == CAM_CTRL_OK && v == 2);
    CHECK(pv(2, "home", &v) == CAM_CTRL_OK && v == 4);
    CHECK(pv(2, "3", &v) == CAM_CTRL_OK && v == 3);
    CHECK(pv(2, "5", &v) == CAM_CTRL_E_RANGE);
    CHECK(pv(2, "Office", &v) == CAM_CTRL_E_SYNTAX);
    CHECK(pv(2, "offic", &v) == CAM_CTRL_E_SYNTAX);

    /* embedded NUL is not the end of the value */
    CHECK(cam_ctrl_parse_value(&k_table[0], "1\0", 2, &v) == CAM_CTRL_E_SYNTAX);
}

static void test_batch(void)
{
    cam_ctrl_batch_t b;
    cam_ctrl_batch_init(&b);

    CHECK(cam_ctrl_batch_add(&b, k_table, N_TABLE, "brightness", 10, "2", 1) == CAM_CTRL_OK);
    CHECK(cam_ctrl_batch_add(&b, k_table, N_TABLE, "awb", 3, "cloudy", 6) == CAM_CTRL_OK);
    CHECK(b.n == 2 && b.item[0].idx == 0 && b.item[0].value == 2 &&
          b.item[1].idx == 2 && b.item[1].value == 3);

    /* every error leaves the batch as it was */
    cam_ctrl_batch_t before = b;
    CHECK(cam_ctrl_batch_add(&b, k_table, N_TABLE, "brightness", 10, "1", 1) == CAM_CTRL_E_DUP);
    CHECK(cam_ctrl_batch_add(&b, k_table, N_TABLE, "zoom", 4, "1", 1) == CAM_CTRL_E_UNKNOWN);
    CHECK(cam_ctrl_batch_add(&b, k_table, N_TABLE, "quality", 7, "300", 3) == CAM_CTRL_E_RANGE);
    CHECK(cam_ctrl_batch_add(&b, k_table, N_TABLE, "quality", 7, "x", 1) == CAM_CTRL_E_SYNTAX);
    CHECK(memcmp(&b, &before, sizeof(b)) == 0);

    /* capacity: a table bigger than the batch */
    static cam_ctrl_desc_t big[CAM_CTRL_MAX];
    static char names[CAM_CTRL_MAX][8];
    for (unsigned i = 0; i < CAM_CTRL_MAX; i++) {
        snprintf(names[i], sizeof(names[i]), "c%u", i);
        big[i] = (cam_ctrl_desc_t){ names[i], names[i], CAM_CTRL_INT, 0, 0, 10, NULL, i };
    }
    cam_ctrl_batch_init(&b);
    for (unsigned i = 0; i < CAM_CTRL_BATCH_MAX; i++) {
        CHECK(cam_ctrl_batch_add(&b, big, CAM_CTRL_MAX, names[i], strlen(names[i]), "1", 1)
              == CAM_CTRL_OK);
    }
    before = b;
    CHECK(cam_ctrl_batch_add(&b, big, CAM_CTRL_MAX, names[CAM_CTRL_BATCH_MAX],
                             strlen(names[CAM_CTRL_BATCH_MAX]), "1", 1) == CAM_CTRL_E_TOO_MANY);
    CHECK(memcmp(&b, &before, sizeof(b)) == 0);
}

/* A setter that fails on a chosen call and logs the order of writes. */
static int      s_fail_on = -1;
static unsigned s_calls;
static unsigned s_log_idx[CAM_CTRL_BATCH_MAX];

static int fake_set(unsigned idx, int16_t value)
{
    (void)value;
    unsigned call = s_calls++;
    if (call < CAM_CTRL_BATCH_MAX) s_log_idx[call] = idx;
    return ((int)call == s_fail_on) ? -7 : 0;
}

static void test_run_and_record(void)
{
    cam_ctrl_batch_t b;
    cam_ctrl_result_t res;
    cam_ctrl_written_t w;
    int16_t v;

    cam_ctrl_batch_init(&b);
    cam_ctrl_batch_add(&b, k_table, N_TABLE, "quality", 7, "40", 2);
    cam_ctrl_batch_add(&b, k_table, N_TABLE, "flip", 4, "1", 1);
    cam_ctrl_batch_add(&b, k_table, N_TABLE, "brightness", 10, "-3", 2);

    /* all succeed, in request order */
    s_fail_on = -1; s_calls = 0;
    cam_ctrl_run(&b, fake_set, &res);
    CHECK(res.n_ok == 3 && res.failed_at == -1 && res.err == 0);
    CHECK(s_calls == 3 && s_log_idx[0] == 3 && s_log_idx[1] == 1 && s_log_idx[2] == 0);

    cam_ctrl_written_clear(&w);
    CHECK(!cam_ctrl_written_get(&w, 3, &v));          /* nothing before a write */
    cam_ctrl_written_record(&w, &b, &res);
    CHECK(cam_ctrl_written_get(&w, 3, &v) && v == 40);
    CHECK(cam_ctrl_written_get(&w, 0, &v) && v == -3);
    CHECK(!cam_ctrl_written_get(&w, 2, &v));          /* never written */

    /* second write fails: first recorded, second and third not, third never tried */
    cam_ctrl_batch_init(&b);
    cam_ctrl_batch_add(&b, k_table, N_TABLE, "quality", 7, "90", 2);
    cam_ctrl_batch_add(&b, k_table, N_TABLE, "awb", 3, "sunny", 5);
    cam_ctrl_batch_add(&b, k_table, N_TABLE, "brightness", 10, "1", 1);
    s_fail_on = 1; s_calls = 0;
    cam_ctrl_run(&b, fake_set, &res);
    CHECK(res.n_ok == 1 && res.failed_at == 1 && res.err == -7);
    CHECK(s_calls == 2);
    cam_ctrl_written_record(&w, &b, &res);
    CHECK(cam_ctrl_written_get(&w, 3, &v) && v == 90);  /* updated */
    CHECK(!cam_ctrl_written_get(&w, 2, &v));            /* failed: still unknown */
    CHECK(cam_ctrl_written_get(&w, 0, &v) && v == -3);  /* not attempted: old value kept */

    /* first write fails: nothing recorded */
    cam_ctrl_written_t w2;
    cam_ctrl_written_clear(&w2);
    s_fail_on = 0; s_calls = 0;
    cam_ctrl_run(&b, fake_set, &res);
    CHECK(res.n_ok == 0 && res.failed_at == 0 && s_calls == 1);
    cam_ctrl_written_record(&w2, &b, &res);
    CHECK(w2.mask == 0);

    /* forget */
    cam_ctrl_written_clear(&w);
    CHECK(w.mask == 0 && !cam_ctrl_written_get(&w, 3, &v));
    CHECK(!cam_ctrl_written_get(&w, CAM_CTRL_MAX, &v));

    /* empty batch writes nothing */
    cam_ctrl_batch_init(&b);
    s_fail_on = -1; s_calls = 0;
    cam_ctrl_run(&b, fake_set, &res);
    CHECK(res.n_ok == 0 && res.failed_at == -1 && s_calls == 0);
}

/* Random text never yields a value outside the range. */
static void test_fuzz(void)
{
    static const char alphabet[] = "0123456789-+ ax.\0officeautohome";
    char s[12];
    unsigned seed = 12345u;
    for (int round = 0; round < 50000; round++) {
        seed = seed * 1103515245u + 12345u;
        size_t len = (seed >> 16) % sizeof(s);
        for (size_t i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            s[i] = alphabet[(seed >> 16) % (sizeof(alphabet) - 1)];
        }
        for (unsigned c = 0; c < N_TABLE; c++) {
            int16_t v = -32000;
            cam_ctrl_err_t e = cam_ctrl_parse_value(&k_table[c], s, len, &v);
            if (e == CAM_CTRL_OK) {
                CHECK(v >= k_table[c].min && v <= k_table[c].max);
            } else {
                CHECK(v == -32000);
            }
        }
    }
}

int main(void)
{
    test_find();
    test_values();
    test_batch();
    test_run_and_record();
    test_fuzz();
    printf("cam_ctrl_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
