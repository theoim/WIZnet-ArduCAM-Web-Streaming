/**
 * @file    arducam_qb.c
 * @brief   Arducam Quick-Bootup 3MP DVP (B0554) driver - the LH water tank camera.
 *
 * This module does not expose its image sensor. The host talks to a controller
 * on the module (SCCB 0x1F, 16-bit register addresses) and writes one register
 * to change resolution or format; the controller configures the sensor behind
 * it. The sensor's part number is not published.
 *
 * Measured with camera_probe on 2026-10-22:
 *
 *   SCCB scan               0x1F only
 *   SENSOR_ID  0x0000-01    0x039E
 *   FIRMWARE   0x0002       0x02
 *   PIXEL_FMT  0x0120       0x01 (JPEG)
 *   RESOLUTION 0x0121       0x03 (1280x720)
 *   FOCUS      0x0129       0xFF (fixed focus - reads as "not implemented")
 *
 * Moved from WIZnet_ArduCAMMega_TOE_Web_Streaming/arducam_mega/arducam_mega.c,
 * which the LH build compiles. The resolution codes, the per-resolution clock
 * dividers and their measurements are that file's; the plumbing follows
 * cam_driver.h and capture moved to capture/dvp_capture.c.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "cam_driver.h"
#include "cam_sccb.h"
#include "dvp_capture.h"
#include CAM_MODULE_HEADER

/* ---- controller register map (mega_ccm_regs.h) ---- */
#define QB_SCCB_ADDR            0x1F
#define QB_REG_SENSOR_ID_H      0x0000
#define QB_REG_SENSOR_ID_L      0x0001
#define QB_REG_FIRMWARE_VER     0x0002
#define QB_REG_CAMERA_RST       0x0102
#define QB_REG_PIXEL_FMT        0x0120
#define QB_REG_RESOLUTION       0x0121
#define QB_REG_SYS_CLK_DIV      0x0200
#define QB_REG_SYS_PLL_DIV      0x0201

/* The ID this driver was written against. Another Arducam module on the same
 * address (5MP B0555, a Mega) may use the same map - or not. */
#define QB_EXPECTED_SENSOR_ID   0x039Eu

const cam_caps_t cam_caps = {
    .name        = "Arducam Quick-Bootup 3MP DVP",
    .has_jpeg    = true,
    .mono        = false,
    /* What the LH build ships at. */
    .default_res = RES_1280X720,
};

volatile uint8_t cam_clk_div = 0x02;
volatile uint8_t cam_pll_div = 0x01;

static bool s_found;

static int wr(uint16_t reg, uint8_t val)
{
    return sccb_write16(QB_SCCB_ADDR, reg, val) ? 1 : 0;
}

bool cam_sensor_found(void)
{
    return s_found;
}

bool cam_sensor_supports(res_t res)
{
    switch (res) {
    case RES_320X240: case RES_640X480: case RES_1280X720:
    case RES_1600X1200: case RES_1920X1080:
        return true;
    default:
        return false;
    }
}

/* Push the current dividers. The resolution path and runtime tuning write the
 * same two registers the same way. */
static int qb_apply_clock_div(uint32_t settle_ms)
{
    int ret = wr(QB_REG_SYS_CLK_DIV, cam_clk_div);
    sleep_ms(settle_ms);
    ret += wr(QB_REG_SYS_PLL_DIV, cam_pll_div);
    sleep_ms(settle_ms);
    return ret;
}

/*
 * A divider of 0 stops the sensor clock: VSYNC stays high, no frame is ever
 * emitted, and writing a valid divider afterwards does not restart it - the
 * sensor has to be reset. Reject 0 rather than walk into a state that needs a
 * power cycle.
 */
int cam_set_clock_div(uint8_t clk_div, uint8_t pll_div)
{
    if (clk_div == 0 || pll_div == 0) {
        printf("Sensor clock: rejected CLK_DIV=0x%02X PLL_DIV=0x%02X"
               " (0 stops the clock)\n", clk_div, pll_div);
        return -1;
    }
    cam_clk_div = clk_div;
    cam_pll_div = pll_div;
    printf("Sensor clock: CLK_DIV=0x%02X PLL_DIV=0x%02X\n", clk_div, pll_div);
    return qb_apply_clock_div(10);
}

static int qb_reset(void)
{
    int ret = wr(QB_REG_CAMERA_RST, 0x00);
    ret += wr(QB_REG_CAMERA_RST, 0x01);
    sleep_ms(1000);
    return ret;
}

/*
 * Per-resolution sensor clock divider. Measured on this module:
 *
 *   320x240    1 halves readout to 29 ms but tears the picture: bands of the
 *              image shift sideways where bytes went missing. 2 is clean.
 *   640x480    1 is clean by eye - 29 ms readout, 22.2 fps. On 2026-10-22 the
 *              page reopened the stream every ~10 s during an aging run and
 *              this was first blamed on damaged frames at divider 1, so 2
 *              was tried: 59 ms readout, 14.0 fps. The reopen reasons then
 *              showed r=idle 214 / r=err 0 - the page tab was in the
 *              background, not the frames broken. Back to 1; r_err in
 *              /api/status over a full aging run is the test still owed.
 *   1280x720   1 gives 22 ms against 44 ms at 2, and stays clean.
 *   1600x1200  2 for now. 1 was unusable before the capture loop stopped
 *   1920x1080  copying every line, and has not been retested since.
 */
static uint8_t clk_div_for_res(res_t res)
{
    switch (res) {
    case RES_640X480:
    case RES_1280X720:
        return 0x01;
    default:
        return 0x02;
    }
}

#if CAM_QB_DIAG
/*
 * Phase 2b diagnostic - READ ONLY. Nothing here writes to the module.
 *
 * mega_ccm_regs.h names these registers, but nothing in this repository says
 * what values they take or whether this module (SENSOR_ID 0x039E) implements
 * them. FOCUS 0x0129 already reads 0xFF on this fixed-focus module, which
 * suggests 0xFF is how the controller answers for a register it does not
 * have. That is a hint, not a rule: a register can read 0xFF and still be
 * writable, or read a real value and do nothing. So this only logs; deciding
 * which ones become controls waits for a write test with the picture in view.
 *
 * Each register is read twice; a difference between the reads is reported.
 */
static const struct {
    uint16_t    reg;
    const char *name;                /* mega_ccm_regs.h name, or why not */
} k_diag_regs[] = {
    { 0x0122, "BRIGHTNESS" },
    { 0x0123, "CONTRAST" },
    { 0x0124, "SATURATION" },
    { 0x0125, "EXP_COMPENSATE" },
    { 0x0126, "AWB_MODE" },
    { 0x0127, "SPECIAL" },
    { 0x0128, "SHARPNESS" },
    { 0x0129, "FOCUS" },
    { 0x012A, "IMAGE_QUALITY" },
    { 0x012B, "IMAGE_FLIP" },
    { 0x012C, "IMAGE_MIRROR" },
    { 0x0130, "AGC_MODE" },
    { 0x0131, "MANUAL_AGC" },
    { 0x0132, "(no name in mega_ccm_regs.h)" },
    { 0x0133, "MANUAL_EXP_H" },
    { 0x0134, "MANUAL_EXP_L" },
};

static void qb_diag_read(const char *when)
{
    unsigned ff = 0, other = 0, bad = 0;

    printf("[qb-diag] %s - read only; names are mega_ccm_regs.h's, not proof of function\n",
           when);
    for (unsigned i = 0; i < sizeof(k_diag_regs) / sizeof(k_diag_regs[0]); i++) {
        uint8_t a = 0, b = 0;
        int ea = sccb_read16(QB_SCCB_ADDR, k_diag_regs[i].reg, &a);
        int eb = sccb_read16(QB_SCCB_ADDR, k_diag_regs[i].reg, &b);
        const char *note;
        if (ea || eb) {
            note = "read failed";
            bad++;
        } else if (a != b) {
            note = "two reads differ";
            bad++;
        } else if (a == 0xFF) {
            note = "0xFF - reads like FOCUS (not implemented?)";
            ff++;
        } else {
            note = "value - candidate, unconfirmed";
            other++;
        }
        if (ea || eb) {
            printf("[qb-diag]   0x%04X %-28s  --          %s\n",
                   k_diag_regs[i].reg, k_diag_regs[i].name, note);
        } else {
            printf("[qb-diag]   0x%04X %-28s  0x%02X 0x%02X   %s\n",
                   k_diag_regs[i].reg, k_diag_regs[i].name, a, b, note);
        }
    }
    printf("[qb-diag] %u read a value, %u read 0xFF, %u failed/unstable\n", other, ff, bad);
}

/*
 * Phase 2b write test, diagnostic image only (GET /api/diag/reg). Limited to
 * the control block 0x0122-0x0134 so that reset (0x0102), format, resolution
 * and the clock dividers (0x0200) cannot be hit by a typo. Caller owns the
 * sensor (server pauses core 1 first).
 *
 * Returns 0 ok, -1 address outside the block, -2 SCCB error.
 */
int cam_qb_diag_reg(uint16_t reg, bool do_write, uint8_t val,
                    uint8_t *before, uint8_t *after)
{
    if (reg < 0x0122 || reg > 0x0134) {
        return -1;
    }
    if (sccb_read16(QB_SCCB_ADDR, reg, before)) {
        return -2;
    }
    if (do_write) {
        if (wr(reg, val)) {
            return -2;
        }
        sleep_ms(50);
    }
    if (sccb_read16(QB_SCCB_ADDR, reg, after)) {
        return -2;
    }
    if (do_write) {
        printf("[qb-diag] write 0x%04X: before 0x%02X, wrote 0x%02X, read back 0x%02X\n",
               reg, *before, val, *after);
    } else {
        printf("[qb-diag] read 0x%04X: 0x%02X\n", reg, *after);
    }
    return 0;
}
#endif /* CAM_QB_DIAG */

int cam_set_framesize(res_t res)
{
    uint8_t  code;
    uint32_t settle_ms;

    switch (res) {
    case RES_320X240:   code = 0x01; settle_ms = 10;  break;
    case RES_640X480:   code = 0x02; settle_ms = 10;  break;
    case RES_1280X720:  code = 0x03; settle_ms = 10;  break;
    case RES_1600X1200: code = 0x04; settle_ms = 100; break;
    case RES_1920X1080: code = 0x05; settle_ms = 100; break;
    default:
        printf("cam_set_framesize: %d is not a size this module offers\n", (int)res);
        return -1;
    }

    camera.res = res;
    /* A divider set by hand was chosen for the previous mode; adopt the tuned
     * one for this mode. */
    cam_clk_div = clk_div_for_res(res);

    int ret = wr(QB_REG_RESOLUTION, code);

    /*
     * Give the controller time to finish switching modes before touching the
     * clock. With only the short settle the divider write was silently ignored
     * at the lower resolutions. The LH build also learned that a size write
     * straight after init is dropped without this - check the frame size in KB,
     * not the console, to know what the module actually did.
     */
    sleep_ms(settle_ms < 100 ? 100 : settle_ms);
    ret += qb_apply_clock_div(settle_ms);
#if CAM_QB_DIAG
    {
        static const char *const nm[] = { "?", "?", "?", "320x240", "?", "640x480", "?",
                                          "?", "1280x720", "?", "1600x1200", "1920x1080" };
        char when[48];
        snprintf(when, sizeof(when), "after frame size %s (rc %d)",
                 (unsigned)res < sizeof(nm) / sizeof(nm[0]) ? nm[res] : "?", ret);
        qb_diag_read(when);
    }
#endif
    return ret;
}

int cam_set_pixformat(pixfmt_t fmt)
{
    /* RGB565 and YUV422 exist in the controller but need a row-counting
     * capture path; refused rather than silently treated as JPEG. */
    if (fmt != PIXFORMAT_JPEG) {
        printf("cam_set_pixformat: only JPEG is wired up for this module\n");
        return -1;
    }
    camera.pixfmt = PIXFORMAT_JPEG;
    return wr(QB_REG_PIXEL_FMT, 0x01);
}

/*
 * Image controls, from the write tests on 2026-10-26 (diagnostic image,
 * GET /api/diag/reg, LH module SENSOR_ID 0x039E):
 *
 *   flip    0x012B  0/1 turns the picture upside down and back. Reads 0xFF
 *   mirror  0x012C  0/1 left-right and back. Reads 0xFF       whatever is written.
 *   brightness 0x0122, saturation 0x0124, white balance 0x0126: a visible
 *           change at 1. contrast 0x0123: frame size moves with the value
 *           (odd larger, even smaller).
 *   A sweep over the ranges below (test/qb_ctrl_sweep.py) never stopped or
 *   corrupted capture: 30 fps throughout, no drops beyond the background.
 *
 * Brightness, contrast and saturation are not a scale 0..max. Checked by eye
 * on the stream (2026-10-26) for all three: 0 is the default, odd codes go
 * one way and even codes the other - brightness 3 brighter, 4 darker, 5
 * brighter; contrast odd sharper, even flatter; saturation odd stronger, even
 * weaker. That matches the contrast sweep (odd larger frames, even smaller).
 * So the code is 0, +1, -1, +2, -2, ... and the table offers the signed level;
 * qb_level_code() turns it into the register value. How big one step is has
 * not been measured, so they stay PROVISIONAL.
 *
 * White balance is a mode number whose modes are not identified (1 gives a
 * red cast), and it reads back a different value than written.
 *
 * Left out: sharpness 0x0128 (no visible change), and image quality 0x012A -
 * several values stopped capture until core 1 reset the sensor, and the value
 * written survived a full power cycle. The module appears to keep at least
 * some settings in non-volatile memory.
 */
static const char *const k_awb_modes[] = { "mode 0", "mode 1", "mode 2", "mode 3", "mode 4" };

/* priv = register address, plus this bit when the value is a signed level */
#define QB_LEVEL  0x10000u

static const cam_ctrl_desc_t k_ctrls[] = {
    /* name          label            type           flags                   min max  enum         reg */
    { "brightness", "Brightness",    CAM_CTRL_INT,  CAM_CTRL_F_PROVISIONAL, -4, 4,   NULL,        0x0122 | QB_LEVEL },
    { "contrast",   "Contrast",      CAM_CTRL_INT,  CAM_CTRL_F_PROVISIONAL, -3, 3,   NULL,        0x0123 | QB_LEVEL },
    { "saturation", "Saturation",    CAM_CTRL_INT,  CAM_CTRL_F_PROVISIONAL, -3, 3,   NULL,        0x0124 | QB_LEVEL },
    { "awb",        "White balance", CAM_CTRL_ENUM, CAM_CTRL_F_PROVISIONAL, 0,  4,   k_awb_modes, 0x0126 },
    { "flip",       "Flip",          CAM_CTRL_BOOL, 0,                      0,  1,   NULL,        0x012B },
    { "mirror",     "Mirror",        CAM_CTRL_BOOL, 0,                      0,  1,   NULL,        0x012C },
};
#define QB_CTRL_COUNT ((unsigned)(sizeof(k_ctrls) / sizeof(k_ctrls[0])))

/* Signed level -> register code: 0 -> 0, +n -> 2n-1, -n -> 2n. */
static uint8_t qb_level_code(int16_t level)
{
    return level > 0 ? (uint8_t)(2 * level - 1) : (uint8_t)(-2 * level);
}

const cam_ctrl_desc_t *cam_sensor_controls(unsigned *count)
{
    *count = QB_CTRL_COUNT;
    return k_ctrls;
}

int cam_sensor_set_control(unsigned idx, int16_t value)
{
    if (idx >= QB_CTRL_COUNT || value < k_ctrls[idx].min || value > k_ctrls[idx].max) {
        return -1;                   /* cam_ctrl checked already; never write past it */
    }
    uint32_t priv = k_ctrls[idx].priv;
    uint8_t  code = (priv & QB_LEVEL) ? qb_level_code(value) : (uint8_t)value;
    return wr((uint16_t)(priv & 0xFFFFu), code);
}

int cam_recover(void)
{
    printf("Sensor recovery: resetting...\n");
    int ret = qb_reset();
    sleep_ms(500);
    ret += cam_set_pixformat(camera.pixfmt);
    sleep_ms(200);
    ret += cam_set_framesize(camera.res);      /* re-applies clock dividers */
    sleep_ms(200);
    printf("Sensor recovery: done (res=%d, CLK_DIV=0x%02X, PLL_DIV=0x%02X)\n",
           (int)camera.res, cam_clk_div, cam_pll_div);
    return ret;
}

void cam_sensor_init(void)
{
    /* PWDN is active high. The inherited driver wrote it without claiming the
     * pin or setting a direction - the write went nowhere and the module ran
     * on whatever the board's default was. Driven properly here. */
    gpio_init(CAM_PIN_PWDN);
    gpio_set_dir(CAM_PIN_PWDN, GPIO_OUT);
    gpio_put(CAM_PIN_PWDN, 0);

    /* The module claims a 300 ms boot. */
    sleep_ms(500);
    sccb_init(i2c0, CAM_PIN_SDA, CAM_PIN_SCL, CAM_SCCB_HZ);

    uint8_t h = 0, l = 0, fw = 0;
    int e = sccb_read16(QB_SCCB_ADDR, QB_REG_SENSOR_ID_H, &h) |
            sccb_read16(QB_SCCB_ADDR, QB_REG_SENSOR_ID_L, &l) |
            sccb_read16(QB_SCCB_ADDR, QB_REG_FIRMWARE_VER, &fw);
    uint16_t id = (uint16_t)((h << 8) | l);

    s_found = (e == 0 && id == QB_EXPECTED_SENSOR_ID);
    if (e) {
        printf("Arducam controller NOT FOUND on SCCB 0x%02X. Check 3V3 and PWDN"
               " low on GP%d; flash camera_probe to see what is there.\n",
               QB_SCCB_ADDR, CAM_PIN_PWDN);
    } else if (!s_found) {
        printf("Arducam controller answers but SENSOR_ID 0x%04X FW 0x%02X is not"
               " the 0x%04X this driver was measured against - a different"
               " module? Continuing with the same register map.\n",
               id, fw, QB_EXPECTED_SENSOR_ID);
    } else {
        printf("Arducam Quick-Bootup 3MP found: SENSOR_ID 0x%04X FW 0x%02X\n", id, fw);
    }

    printf("Resetting camera...\n");
    qb_reset();
    sleep_ms(500);
    printf("Setting JPEG format...\n");
    cam_set_pixformat(PIXFORMAT_JPEG);
    sleep_ms(200);
    printf("Setting initial resolution...\n");
    cam_set_framesize(cam_caps.default_res);
    sleep_ms(200);
    printf("Camera sensor initialization complete\n");
}
