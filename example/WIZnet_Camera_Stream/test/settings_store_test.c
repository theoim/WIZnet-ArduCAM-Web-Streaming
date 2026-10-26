/*
 * Host tests for settings/settings_store.c against a RAM model of NOR flash:
 * erase sets a sector to 0xFF, program can only clear bits, both must be
 * aligned. A power cut is simulated by letting the model stop after a given
 * number of flash operations - and, for a program, after a given number of
 * bytes of that page. Built and run by run_host_tests.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_store.h"

static int g_checks, g_fail;

#define CHECK(cond) do {                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_fail++;                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                   \
    } while (0)

/* ------------------------------ flash model ------------------------------- */
static uint8_t g_flash[2 * SSTORE_SECTOR];
static int     g_ops_left = -1;     /* -1: unlimited. 0: power is gone       */
static int     g_cut_bytes;         /* bytes of the interrupted page written */
static int     g_ops;               /* operations performed                  */
static int     g_bad_align;
static int     g_fail_reads;        /* reads that return an error            */

static int f_read(uint32_t off, void *buf, uint32_t len)
{
    if (g_fail_reads) return -1;
    if (off + len > sizeof(g_flash)) return -1;
    memcpy(buf, g_flash + off, len);
    return 0;
}

static int f_erase(uint32_t off)
{
    if (off % SSTORE_SECTOR || off >= sizeof(g_flash)) { g_bad_align++; return -1; }
    if (g_ops_left == 0) return -1;
    g_ops++;
    if (g_ops_left > 0 && --g_ops_left == 0) {
        /* cut during the erase: half the sector erased */
        memset(g_flash + off, 0xFF, SSTORE_SECTOR / 2);
        return -1;
    }
    memset(g_flash + off, 0xFF, SSTORE_SECTOR);
    return 0;
}

static int f_program(uint32_t off, const void *buf, uint32_t len)
{
    if (off % SSTORE_PAGE || len % SSTORE_PAGE || off + len > sizeof(g_flash)) {
        g_bad_align++;
        return -1;
    }
    if (g_ops_left == 0) return -1;
    g_ops++;
    uint32_t n = len;
    int cut = 0;
    if (g_ops_left > 0 && --g_ops_left == 0) {
        n = (uint32_t)g_cut_bytes;
        cut = 1;
    }
    const uint8_t *b = buf;
    for (uint32_t i = 0; i < n; i++) g_flash[off + i] &= b[i];
    return cut ? -1 : 0;
}

static const sstore_flash_t k_flash = { f_read, f_erase, f_program };

static void power_on(void) { g_ops_left = -1; g_ops = 0; }

static void fill(uint8_t *p, uint32_t len, uint32_t seed)
{
    for (uint32_t i = 0; i < len; i++) p[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 8));
}

/* --------------------------------- tests ---------------------------------- */
static void test_crc(void)
{
    CHECK(sstore_crc32("123456789", 9) == 0xCBF43926u);   /* IEEE check value */
    CHECK(sstore_crc32("", 0) == 0);
}

static void test_blank(void)
{
    sstore_t s;
    uint8_t buf[16];
    uint32_t len = 99;
    memset(g_flash, 0xFF, sizeof(g_flash));
    sstore_init(&s, &k_flash);
    CHECK(sstore_load(&s, buf, sizeof(buf), &len, NULL) == SSTORE_E_NONE);
    CHECK(len == 99 && s.cur == -1);
    memset(g_flash, 0x00, sizeof(g_flash));                 /* all zero: not a record */
    CHECK(sstore_load(&s, buf, sizeof(buf), &len, NULL) == SSTORE_E_NONE);
}

static void test_roundtrip(void)
{
    static uint8_t in[SSTORE_PAYLOAD_MAX], out[SSTORE_PAYLOAD_MAX];
    uint32_t sizes[] = { 0, 1, 231, 232, 233, 488, 1000, SSTORE_PAYLOAD_MAX };
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        sstore_t s;
        uint32_t len = 0;
        uint16_t ver = 0;
        memset(g_flash, 0xFF, sizeof(g_flash));
        power_on();
        sstore_init(&s, &k_flash);
        fill(in, sizes[k], k);
        CHECK(sstore_save(&s, in, sizes[k], (uint16_t)(7 + k)) == SSTORE_OK);
        CHECK(g_bad_align == 0);
        sstore_t t;
        sstore_init(&t, &k_flash);
        memset(out, 0, sizeof(out));
        CHECK(sstore_load(&t, out, sizeof(out), &len, &ver) == SSTORE_OK);
        CHECK(len == sizes[k] && ver == 7 + k && memcmp(in, out, len) == 0);
        CHECK(t.cur == 0 && t.gen == 1);
    }
    sstore_t s;
    sstore_init(&s, &k_flash);
    CHECK(sstore_save(&s, in, SSTORE_PAYLOAD_MAX + 1, 1) == SSTORE_E_TOO_BIG);
}

/* Saves alternate slots and the newest generation wins after a reboot. */
static void test_alternate(void)
{
    uint8_t in[100], out[100];
    sstore_t s;
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    sstore_init(&s, &k_flash);
    for (uint32_t i = 1; i <= 6; i++) {
        fill(in, sizeof(in), i);
        CHECK(sstore_save(&s, in, sizeof(in), 1) == SSTORE_OK);
        CHECK(s.cur == (int)((i - 1) % 2) && s.gen == i);
        sstore_t t;
        uint32_t len;
        sstore_init(&t, &k_flash);
        CHECK(sstore_load(&t, out, sizeof(out), &len, NULL) == SSTORE_OK);
        CHECK(t.gen == i && memcmp(in, out, sizeof(in)) == 0);
    }
}

/* save() without load() first must not erase the existing record. */
static void test_save_without_load(void)
{
    uint8_t a[50], b[50], out[50];
    sstore_t s, t;
    uint32_t len;
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    sstore_init(&s, &k_flash);
    fill(a, sizeof(a), 1);
    CHECK(sstore_save(&s, a, sizeof(a), 1) == SSTORE_OK);     /* slot 0, gen 1 */
    sstore_init(&t, &k_flash);                                /* fresh, no load */
    fill(b, sizeof(b), 2);
    CHECK(sstore_save(&t, b, sizeof(b), 1) == SSTORE_OK);
    CHECK(t.cur == 1 && t.gen == 2);                          /* went to slot 1 */
    sstore_init(&s, &k_flash);
    CHECK(sstore_load(&s, out, sizeof(out), &len, NULL) == SSTORE_OK);
    CHECK(memcmp(out, b, sizeof(b)) == 0);
}

/*
 * The core property: cut the power at every operation of a save, and at
 * several byte positions inside each page program. After "reboot" the load
 * must return either the old record or the new one - never anything else -
 * and a following save must work.
 */
static void test_power_cut(void)
{
    static uint8_t old_p[600], new_p[600], out[600];
    int cases = 0, saw_old = 0, saw_new = 0;
    int cut_bytes[] = { 0, 1, 4, 100, 255 };

    for (int nb = 0; nb < (int)(sizeof(cut_bytes) / sizeof(cut_bytes[0])); nb++) {
        for (int cut_at = 1; cut_at < 40; cut_at++) {
            sstore_t s, t;
            uint32_t len = 0;
            memset(g_flash, 0xFF, sizeof(g_flash));
            power_on();
            sstore_init(&s, &k_flash);
            fill(old_p, sizeof(old_p), 11);
            CHECK(sstore_save(&s, old_p, sizeof(old_p), 1) == SSTORE_OK);   /* gen 1 */
            fill(old_p, sizeof(old_p), 12);
            CHECK(sstore_save(&s, old_p, sizeof(old_p), 1) == SSTORE_OK);   /* gen 2: old */

            fill(new_p, sizeof(new_p), 13);
            g_ops = 0;
            g_ops_left = cut_at;
            g_cut_bytes = cut_bytes[nb];
            sstore_err_t e = sstore_save(&s, new_p, sizeof(new_p), 1);
            int finished = (e == SSTORE_OK);

            power_on();
            sstore_init(&t, &k_flash);
            sstore_err_t le = sstore_load(&t, out, sizeof(out), &len, NULL);
            CHECK(le == SSTORE_OK);
            int is_old = memcmp(out, old_p, sizeof(out)) == 0;
            int is_new = memcmp(out, new_p, sizeof(out)) == 0;
            CHECK(is_old || is_new);
            CHECK(!finished || is_new);           /* a completed save is never lost */
            CHECK(is_new == (t.gen == 3));
            saw_old += is_old;
            saw_new += is_new;
            cases++;

            fill(new_p, sizeof(new_p), 14);       /* the store keeps working */
            CHECK(sstore_save(&t, new_p, sizeof(new_p), 1) == SSTORE_OK);
            sstore_init(&s, &k_flash);
            CHECK(sstore_load(&s, out, sizeof(out), &len, NULL) == SSTORE_OK);
            CHECK(memcmp(out, new_p, sizeof(out)) == 0);
            if (finished) break;                  /* later cut points never reached */
        }
    }
    printf("power cut: %d cases, %d kept old, %d had new\n", cases, saw_old, saw_new);
    CHECK(saw_old > 0 && saw_new > 0);
}

/* Any single flipped bit in the newest record falls back to the older one. */
static void test_corruption(void)
{
    static uint8_t a[300], b[300], out[300];
    static uint8_t snap[sizeof(g_flash)];
    sstore_t s;
    uint32_t len;
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    sstore_init(&s, &k_flash);
    fill(a, sizeof(a), 21);
    CHECK(sstore_save(&s, a, sizeof(a), 1) == SSTORE_OK);   /* slot 0 */
    fill(b, sizeof(b), 22);
    CHECK(sstore_save(&s, b, sizeof(b), 1) == SSTORE_OK);   /* slot 1, newest */
    memcpy(snap, g_flash, sizeof(g_flash));

    uint32_t base = SSTORE_SECTOR;
    uint32_t spots[] = { 0, 5, 9, 13, 17, 21, 24, 100, 323, SSTORE_SECTOR - SSTORE_PAGE,
                         SSTORE_SECTOR - SSTORE_PAGE + 3 };
    for (unsigned i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) {
        for (int bit = 0; bit < 8; bit += 3) {
            memcpy(g_flash, snap, sizeof(g_flash));
            g_flash[base + spots[i]] ^= (uint8_t)(1u << bit);
            sstore_init(&s, &k_flash);
            CHECK(sstore_load(&s, out, sizeof(out), &len, NULL) == SSTORE_OK);
            CHECK(s.cur == 0 && memcmp(out, a, sizeof(a)) == 0);
        }
    }
    /* a flip past the payload, in the erased padding, changes nothing */
    memcpy(g_flash, snap, sizeof(g_flash));
    g_flash[base + 1000] ^= 0x01;
    sstore_init(&s, &k_flash);
    CHECK(sstore_load(&s, out, sizeof(out), &len, NULL) == SSTORE_OK && s.cur == 1);
}

static void test_gen_wrap(void)
{
    uint8_t p[10] = { 0 }, out[10];
    sstore_t s, t;
    uint32_t len;
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    sstore_init(&s, &k_flash);
    CHECK(sstore_save(&s, p, sizeof(p), 1) == SSTORE_OK);
    s.gen = 0xFFFFFFFEu;                      /* pretend many saves happened */
    p[0] = 1;
    CHECK(sstore_save(&s, p, sizeof(p), 1) == SSTORE_OK && s.gen == 0xFFFFFFFFu);
    p[0] = 2;
    CHECK(sstore_save(&s, p, sizeof(p), 1) == SSTORE_OK && s.gen == 1);   /* skips 0 */
    sstore_init(&t, &k_flash);
    CHECK(sstore_load(&t, out, sizeof(out), &len, NULL) == SSTORE_OK);
    CHECK(out[0] == 2 && t.gen == 1);         /* 1 is newer than 0xFFFFFFFF */
}

static void test_errors(void)
{
    uint8_t p[40] = { 1 }, small[8];
    sstore_t s;
    uint32_t len = 0;
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    sstore_init(&s, &k_flash);
    CHECK(sstore_save(&s, p, sizeof(p), 1) == SSTORE_OK);
    sstore_init(&s, &k_flash);
    CHECK(sstore_load(&s, small, sizeof(small), &len, NULL) == SSTORE_E_TOO_BIG);
    CHECK(s.cur == 0);                        /* still remembered for the next save */
    g_fail_reads = 1;
    sstore_init(&s, &k_flash);
    CHECK(sstore_load(&s, p, sizeof(p), &len, NULL) == SSTORE_E_NONE);
    g_fail_reads = 0;
    g_ops_left = 0;                           /* every write fails */
    sstore_init(&s, &k_flash);
    CHECK(sstore_save(&s, p, sizeof(p), 1) == SSTORE_E_FLASH);
    power_on();
    CHECK(sstore_err_name(SSTORE_E_VERIFY)[0] != '?');
}

int main(void)
{
    test_crc();
    test_blank();
    test_roundtrip();
    test_alternate();
    test_save_without_load();
    test_power_cut();
    test_corruption();
    test_gen_wrap();
    test_errors();
    printf("settings_store_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
