/**
 * @file    ov2640.c
 * @brief   OV2640 sensor driver for the camera library (cam_driver.h).
 *
 * Moved from WIZnet_OV2640_Exhibition/ov2640.c. The register tables, the
 * native-SVGA 800x600 table and every measured note are the exhibition's; what
 * changed is the plumbing:
 *
 *   - SCCB goes through cam_sccb (8-bit register addresses, with timeouts)
 *   - pins and XCLK come from the module header, not constants in this file
 *   - capture (PIO/DMA/JPEG end) moved to capture/dvp_capture.c, shared
 *   - public names follow cam_driver.h: cam_set_framesize(), cam_recover() ...
 *
 * Baseline to compare against after the move: 800x600 JPEG ~22 KB, TOE 17 fps
 * (exhibition build 68ac5e3, 2026-10-07).
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "cam_driver.h"
#include "cam_sccb.h"
#include "dvp_capture.h"
#include "ov2640_defs.h"
#include "ov2640_regs.h"
#include CAM_MODULE_HEADER

const cam_caps_t cam_caps = {
    .name        = "OV2640",
    .has_jpeg    = true,
    .mono        = false,
    /* Native SVGA array: 17 fps. Every larger size reads the UXGA array. */
    .default_res = RES_800X600,
};

volatile uint8_t cam_clk_div = 0x01;
volatile uint8_t cam_pll_div = 0x02;

static bool s_found;
static int  ov2640_reset(void);

static uint8_t wr(uint8_t reg, uint8_t val)
{
    return sccb_write8(OV2640_SCCB_ADDR, reg, val) ? 1 : 0;
}

static uint8_t rd(uint8_t reg)
{
    uint8_t v = 0;
    sccb_read8(OV2640_SCCB_ADDR, reg, &v);
    return v;
}

static int ov2640_bank(uint8_t bank)
{
    return wr(OV2640_REG_BANK, bank);
}

static int ov2640_write_table(const ov2640_reg_t *t)
{
    int err = 0;
    for (; !(t->reg == OV2640_TABLE_END_REG && t->val == OV2640_TABLE_END_VAL); t++) {
        err += wr(t->reg, t->val);
    }
    return err;
}

bool cam_sensor_found(void)
{
    return s_found;
}

bool cam_sensor_supports(res_t res)
{
    switch (res) {
    case RES_160X120: case RES_176X144: case RES_320X240: case RES_352X288:
    case RES_640X480: case RES_800X600: case RES_1024X768: case RES_1280X1024:
    case RES_1600X1200:
        return true;
    default:
        return false;
    }
}

/*
 * The master clock. A bare OV2640 has nothing to run on until it is given one,
 * and with no XCLK it does not answer SCCB either - which reads in a log as
 * "I2C is broken". PWM, because the GPOUT-capable pins are taken (GP21 by the
 * W6300, GP23-25 internal to the board).
 */
static void xclk_start(void)
{
#if CAM_PIN_XCLK >= 0
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t top    = sys_hz / CAM_XCLK_HZ - 1u;     /* 200 MHz / 20 MHz -> 9 */

    gpio_set_function(CAM_PIN_XCLK, GPIO_FUNC_PWM);
    /* Default 4 mA with limited slew spends most of a half period on the edge. */
    gpio_set_drive_strength(CAM_PIN_XCLK, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_slew_rate(CAM_PIN_XCLK, GPIO_SLEW_RATE_FAST);

    uint slice = pwm_gpio_to_slice_num(CAM_PIN_XCLK);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, top);
    pwm_init(slice, &c, false);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(CAM_PIN_XCLK), (top + 1u) / 2u);
    pwm_set_enabled(slice, true);
    printf("XCLK: GP%d at %lu Hz\n", CAM_PIN_XCLK, (unsigned long)(sys_hz / (top + 1u)));
#else
    printf("XCLK: none driven - the module must have its own oscillator\n");
#endif
}

void cam_sensor_init(void)
{
    sccb_init(i2c0, CAM_PIN_SDA, CAM_PIN_SCL, CAM_SCCB_HZ);

    /* PWDN is active high, so low is awake. Claimed and driven: the inherited
     * code wrote it without setting a direction, which did nothing. */
    gpio_init(CAM_PIN_PWDN);
    gpio_set_dir(CAM_PIN_PWDN, GPIO_OUT);
    gpio_put(CAM_PIN_PWDN, 0);
#if CAM_PIN_RESET >= 0
    gpio_init(CAM_PIN_RESET);
    gpio_set_dir(CAM_PIN_RESET, GPIO_OUT);
    gpio_put(CAM_PIN_RESET, 1);             /* active low: high is running */
#endif

    /* Before anything is said to the sensor - it has nothing to think with
     * until the clock arrives. */
    xclk_start();
    sleep_ms(10);

    ov2640_bank(OV2640_BANK_SENSOR);
    uint8_t pid = rd(OV2640_REG_PIDH);
    uint8_t ver = rd(OV2640_REG_PIDL);
    s_found = (pid == OV2640_PID_HIGH && ver == OV2640_PID_LOW);
    if (s_found) {
        printf("OV2640 found: PID 0x%02X VER 0x%02X\n", pid, ver);
    } else {
        printf("OV2640 NOT FOUND on SCCB 0x%02X (read PID 0x%02X VER 0x%02X,"
               " expected 0x%02X 0x%02X)\n", OV2640_SCCB_ADDR, pid, ver,
               OV2640_PID_HIGH, OV2640_PID_LOW);
        printf("  Check XCLK on GP%d, PWDN low on GP%d, and 3V3 before"
               " suspecting SIOC/SIOD. Flash camera_probe to see what is there.\n",
               CAM_PIN_XCLK, CAM_PIN_PWDN);
    }

    printf("Resetting camera...\n");
    ov2640_reset();
    printf("Setting JPEG format...\n");
    cam_set_pixformat(PIXFORMAT_JPEG);
    printf("Setting initial resolution...\n");
    cam_set_framesize(cam_caps.default_res);
    printf("Camera sensor initialization complete\n");
}

static int ov2640_apply_clock_div(uint32_t settle_ms)
{
    int ret;

    ov2640_bank(OV2640_BANK_SENSOR);
    ret = wr(OV2640_REG_CLKRC, cam_clk_div);
    sleep_ms(settle_ms);

    ov2640_bank(OV2640_BANK_DSP);
    ret += wr(OV2640_REG_R_DVP_SP, cam_pll_div);
    sleep_ms(settle_ms);

    return ret;
}

/** Read back what the sensor is actually running at, after a table wrote it. */
static void ov2640_read_clock_div(void)
{
    ov2640_bank(OV2640_BANK_SENSOR);
    cam_clk_div = rd(OV2640_REG_CLKRC);

    ov2640_bank(OV2640_BANK_DSP);
    cam_pll_div = rd(OV2640_REG_R_DVP_SP);
}

/*
 * Change the dividers at runtime.
 *
 * The Mega version rejected zero in either field because zero stopped its
 * clock. That rule does not carry over: on an OV2640, CLKRC = 0 means divide by
 * one, and the stock QVGA table writes exactly that. Rejecting it here would
 * refuse a setting the sensor ships with.
 *
 * R_DVP_SP is different - zero there is not a documented divider, and a PCLK
 * that never ticks looks identical to a dead sensor from the capture loop.
 */
int cam_set_clock_div(uint8_t clk_div, uint8_t pll_div)
{
    if (pll_div == 0) {
        printf("Sensor clock: rejected DVP_SP=0 (no PCLK would be emitted)\n");
        return -1;
    }

    cam_clk_div = clk_div;
    cam_pll_div = pll_div;
    printf("Sensor clock: CLKRC=0x%02X DVP_SP=0x%02X\n", clk_div, pll_div);
    return ov2640_apply_clock_div(10);
}

/**
 * Reset the sensor and restore the current format, resolution, and clock.
 * Recovers from a stalled clock without power-cycling the board.
 */
/*
 * Image controls.
 *
 * Values are the OmniVision OV2640 application-note sequences (the same ones
 * esp32-camera ships), not measured here. Brightness, contrast, saturation
 * and the colour effects all live in the DSP's special-digital-effects block,
 * reached indirectly: 0x7C selects an SDE address, each 0x7D write stores one
 * byte there and steps to the next address.
 *
 *   SDE 0x00  enable bits: 0x04 brightness/contrast, 0x02 saturation,
 *             0x40 negative, 0x18 fixed U/V (grey and the tints)
 *   SDE 0x03  U, V saturation           SDE 0x05  fixed U, V for the tints
 *   SDE 0x07  contrast: 0x07..0x0A      SDE 0x09  brightness: 0x09, 0x0A
 *
 * Two things the application note does not settle, so these stay PROVISIONAL:
 *
 *   - Contrast writes 0x09 and 0x0A as well, which is where brightness lives.
 *     The block is rewritten in a fixed order - contrast, then brightness if it
 *     was set - so a brightness level survives a contrast change; whether the
 *     result is right when both are moved is for the eye to judge.
 *   - In the note every sequence writes SDE 0x00 outright, so setting
 *     saturation would switch brightness off. Here the enable byte is the OR of
 *     what has been set.
 *
 * Level 0 is the note's default, which is not quite the boot table (that
 * leaves SDE 0x00 at 0 and 0x09/0x0A at 0x10/0x0E).
 *
 * White balance auto/manual is DSP 0xC7 bit 6 - read-modify-write, because the
 * boot table leaves 0xC7 at 0x10. Flip and mirror are sensor-bank REG04 bits:
 * 0x40|0x10 (vertical flip with the VREF adjust) and 0x80.
 *
 * Every frame-size table rewrites the SDE block, 0xC7 and REG04, so the shadow
 * below is cleared with them (ov2640_ctrl_reset), matching the forget in
 * cam_state.
 */
enum { OVC_BRIGHTNESS, OVC_CONTRAST, OVC_SATURATION, OVC_EFFECT, OVC_AWB, OVC_FLIP, OVC_MIRROR };

static const char *const k_effects[] = { "none", "negative", "grayscale", "red", "green", "blue", "sepia" };
static const char *const k_awb[]     = { "auto", "sunny", "cloudy", "office", "home" };

static const cam_ctrl_desc_t k_ctrls[] = {
    /* name          label            type           flags                   min max enum       id */
    { "brightness", "Brightness",    CAM_CTRL_INT,  CAM_CTRL_F_PROVISIONAL, -2, 2,  NULL,      OVC_BRIGHTNESS },
    { "contrast",   "Contrast",      CAM_CTRL_INT,  CAM_CTRL_F_PROVISIONAL, -2, 2,  NULL,      OVC_CONTRAST },
    { "saturation", "Saturation",    CAM_CTRL_INT,  CAM_CTRL_F_PROVISIONAL, -2, 2,  NULL,      OVC_SATURATION },
    { "effect",     "Effect",        CAM_CTRL_ENUM, CAM_CTRL_F_PROVISIONAL, 0,  6,  k_effects, OVC_EFFECT },
    { "awb",        "White balance", CAM_CTRL_ENUM, CAM_CTRL_F_PROVISIONAL, 0,  4,  k_awb,     OVC_AWB },
    { "flip",       "Flip",          CAM_CTRL_BOOL, 0,                      0,  1,  NULL,      OVC_FLIP },
    { "mirror",     "Mirror",        CAM_CTRL_BOOL, 0,                      0,  1,  NULL,      OVC_MIRROR },
};
#define OV_CTRL_COUNT ((unsigned)(sizeof(k_ctrls) / sizeof(k_ctrls[0])))

/* Per level -2..+2 (index level + 2). */
static const uint8_t k_bright[5]      = { 0x00, 0x10, 0x20, 0x30, 0x40 };            /* SDE 0x09 */
static const uint8_t k_contrast[5][2] = { {0x18, 0x34}, {0x1c, 0x2a}, {0x20, 0x20},
                                          {0x24, 0x16}, {0x28, 0x0c} };               /* SDE 0x08, 0x09 */
static const uint8_t k_sat[5]         = { 0x28, 0x38, 0x48, 0x58, 0x68 };            /* SDE 0x03, 0x04 */
/* enable bits, fixed U, fixed V */
static const uint8_t k_effect[7][3]   = { {0x00, 0x80, 0x80}, {0x40, 0x80, 0x80}, {0x18, 0x80, 0x80},
                                          {0x18, 0x40, 0xc0}, {0x18, 0x40, 0x40}, {0x18, 0xa0, 0x40},
                                          {0x18, 0x40, 0xa6} };
static const uint8_t k_wb[5][3]       = { {0, 0, 0}, {0x5e, 0x41, 0x54}, {0x65, 0x41, 0x4f},
                                          {0x52, 0x41, 0x66}, {0x42, 0x3f, 0x71} };   /* 0xCC..0xCE */

/* What has been set since the last frame-size table; -128 = not set. */
static int8_t s_lvl[3] = { -128, -128, -128 };   /* brightness, contrast, saturation */
static int8_t s_effect = -128;

static void ov2640_ctrl_reset(void)
{
    s_lvl[0] = s_lvl[1] = s_lvl[2] = -128;
    s_effect = -128;
}

static int sde(uint8_t addr, const uint8_t *data, unsigned n)
{
    int err = wr(0x7C, addr);
    for (unsigned i = 0; i < n; i++) {
        err += wr(0x7D, data[i]);
    }
    return err;
}

/* Rewrite the whole SDE block from the shadow. */
static int ov2640_write_sde(void)
{
    uint8_t en = 0;
    int err = ov2640_bank(OV2640_BANK_DSP);

    if (s_lvl[0] != -128 || s_lvl[1] != -128) en |= 0x04;
    if (s_lvl[2] != -128) en |= 0x02;
    if (s_effect != -128) en |= k_effect[s_effect][0];
    err += sde(0x00, &en, 1);

    if (s_lvl[2] != -128) {
        uint8_t uv[2] = { k_sat[s_lvl[2] + 2], k_sat[s_lvl[2] + 2] };
        err += sde(0x03, uv, 2);
    }
    if (s_effect != -128) {
        err += sde(0x05, &k_effect[s_effect][1], 2);
    }
    if (s_lvl[1] != -128) {
        uint8_t c[4] = { 0x20, k_contrast[s_lvl[1] + 2][0], k_contrast[s_lvl[1] + 2][1], 0x06 };
        err += sde(0x07, c, 4);
    }
    if (s_lvl[0] != -128) {
        uint8_t b[2] = { k_bright[s_lvl[0] + 2], 0x00 };
        err += sde(0x09, b, 2);
    }
    return err;
}

const cam_ctrl_desc_t *cam_sensor_controls(unsigned *count)
{
    *count = OV_CTRL_COUNT;
    return k_ctrls;
}

int cam_sensor_set_control(unsigned idx, int16_t value)
{
    if (idx >= OV_CTRL_COUNT || value < k_ctrls[idx].min || value > k_ctrls[idx].max) {
        return -1;
    }
    switch (k_ctrls[idx].priv) {
    case OVC_BRIGHTNESS: s_lvl[0] = (int8_t)value; return ov2640_write_sde();
    case OVC_CONTRAST:   s_lvl[1] = (int8_t)value; return ov2640_write_sde();
    case OVC_SATURATION: s_lvl[2] = (int8_t)value; return ov2640_write_sde();
    case OVC_EFFECT:     s_effect = (int8_t)value; return ov2640_write_sde();
    case OVC_AWB: {
        int err = ov2640_bank(OV2640_BANK_DSP);
        uint8_t c7;
        if (err || sccb_read8(OV2640_SCCB_ADDR, 0xC7, &c7) != 0) {
            return -1;              /* never write back a value that was not read */
        }
        err += wr(0xC7, value ? (uint8_t)(c7 | 0x40) : (uint8_t)(c7 & ~0x40));
        if (value) {
            err += wr(0xCC, k_wb[value][0]);
            err += wr(0xCD, k_wb[value][1]);
            err += wr(0xCE, k_wb[value][2]);
        }
        return err;
    }
    case OVC_FLIP:
    case OVC_MIRROR: {
        uint8_t bits = (k_ctrls[idx].priv == OVC_FLIP) ? 0x50 : 0x80;
        int err = ov2640_bank(OV2640_BANK_SENSOR);
        uint8_t r04;
        if (err || sccb_read8(OV2640_SCCB_ADDR, 0x04, &r04) != 0) {
            return -1;
        }
        err += wr(0x04, value ? (uint8_t)(r04 | bits) : (uint8_t)(r04 & ~bits));
        return err;
    }
    }
    return -1;
}

int cam_recover(void)
{
    int ret;

    printf("Sensor recovery: resetting...\n");
    ret = ov2640_reset();
    sleep_ms(500);
    ret += cam_set_pixformat(camera.pixfmt);
    sleep_ms(200);
    ret += cam_set_framesize(camera.res);   /* re-applies clock dividers */
    sleep_ms(200);
    printf("Sensor recovery: done (res=%d, CLK_DIV=0x%02X, PLL_DIV=0x%02X)\n",
           (int)camera.res, cam_clk_div, cam_pll_div);

    return ret;
}

/*
 * Measured clock settings, overriding what the resolution tables ship with.
 *
 *   CLKRC  R_DVP_SP   result at 800x600
 *   -----  --------   ------------------------------------------------------
 *     1       2       154 ms, 5.9 fps. 7 dropped in 1465 (0.5%). Stable, and
 *                     what the table writes - so nothing is overridden.
 *     0       2        77 ms, 11.9 fps. 2 dropped in 188 (1.1%), and the
 *                     picture goes black on its own after a few minutes: a
 *                     frame corrupt past the SOI check reaches the browser and
 *                     ends the MJPEG stream while the board keeps sending.
 *     0       1       no frames at all - every capture fails the SOI check and
 *                     the recovery path resets the sensor in a loop.
 *
 * Double the clock is outside what the capture path can follow reliably. The
 * failure is not gradual: 1.1% of frames rejected is the visible part, and what
 * is not visible is the fraction that passes every check this code makes and is
 * still wrong. Shipping it would trade a slow picture for an unattended one
 * that stops.
 *
 * The remaining headroom is not in the dividers. Deferring the JPEG marker walk
 * until after the frame would cut the per-chunk work in the capture loop, which
 * is what runs out of time when PCLK doubles - that is the experiment worth
 * doing before touching CLKRC again.
 *
 * So CLKRC is the lever and R_DVP_SP is already as low as this board tolerates.
 * Halving the sensor's internal divider doubles the frame rate; halving the DVP
 * divider on top of that puts PCLK past what the capture path can follow, and
 * the failure is total rather than gradual.
 *
 * ---------------------------------------------------------------------------
 *
 * Getting here took three wrong conclusions, all from the same mistake: reading
 * fps and read_ms immediately after writing the registers. Both are windowed
 * averages over recent frames, so for a second or two after a change they still
 * report the old setting. Three separate combinations were recorded as "no
 * effect" that way, including this one - 0/2 was written off as 154 ms before
 * it turned out to be the answer.
 *
 * If a clock change looks like it did nothing, let it run a few hundred frames
 * before believing the number.
 *
 * ---------------------------------------------------------------------------
 *
 * Only 800x600 is tuned. Every other size keeps the table's values until it has
 * its own measurement - copying this pair across would be guessing, and what it
 * buys when wrong is not a slower picture but no picture.
 *
 * Note what this does NOT fix. Every table sets COM7 to UXGA, so the array
 * reads 1600x1200 whatever the output size, and the frame period is a UXGA
 * frame period. A faster clock makes that frame arrive sooner; it does not make
 * the sensor read fewer pixels. The remaining 4x is in the array mode, not here.
 */
typedef struct {
    bool    tuned;      /* false: leave whatever the table wrote */
    uint8_t clkrc;      /* sensor bank 0x11 */
    uint8_t dvp_sp;     /* DSP bank 0xD3    */
} clk_tuning_t;

static clk_tuning_t tuned_clocks(res_t res)
{
    switch (res) {
    /*
     * Nothing is overridden. CLKRC 0x00 doubles the frame rate and is left out
     * anyway - see the measurements above. The fast setting is still reachable
     * by hand for anyone who wants to look at it:
     *
     *     http://<board>/api/clk?div=0&pll=2
     */
    default:            return (clk_tuning_t){ false, 0, 0 };
    }
}

/*
 * 800x600 read natively, instead of read at 1600x1200 and scaled down.
 *
 * Every table that came with the reference puts COM7 at UXGA and lets the DSP
 * scaler produce the requested size, so the array reads 1,920,000 pixels for
 * every frame whatever the output is. That is why the frame rate was identical
 * at 800x600, 1024x768, 1280x1024 and 1600x1200 - the output size never touched
 * the thing that takes the time.
 *
 * The small tables do not work that way. 320x240 sets COM7 to 0x40, the SVGA
 * array mode, which reads 800x600 - a quarter of the pixels - and scales that
 * down. Measured: 17.1 fps against 5.9 for the same sensor at the same clock.
 *
 * SVGA mode reads 800x600 natively, which is exactly the size wanted here. So
 * this table is the 320x240 one with the scaler taken out of the path:
 *
 *   sensor bank   copied unchanged from OV2640_320x240_JPEG - COM7 0x40 and
 *                 the SVGA window that goes with it
 *   0xc0 / 0xc1   input size, 0x64/0x4b = 800x600. Also unchanged; this is
 *                 what the SVGA array delivers
 *   0x5a / 0x5b   output size. 0x50/0x3c (320x240) becomes 0xc8/0x96, which is
 *                 what the stock 800x600 table asks for - 200 and 150, times
 *                 four
 *   0x51 / 0x52   the scaler window, already 800x600 in the source table
 *
 * Everything else, including CTRLI at 0x50 and the 0xe0 reset bracket, is left
 * at the values both working tables share. They are the same in each despite
 * the tables scaling by different ratios, so they are not the ratio and are not
 * ours to reason about.
 *
 * This is the one table here that no one has shipped. To go back to the stock
 * behaviour, point table_for_res(RES_800X600) at OV2640_800x600_JPEG again -
 * that is the whole revert.
 */
static const ov2640_reg_t OV2640_800x600_SVGA_JPEG[] = {
    /* ---- sensor bank: SVGA array ---- */
    { 0xff, 0x01 }, { 0x12, 0x40 },
    /*
     * CLKRC, pinned.
     *
     * The 320x240 table this was copied from does not write 0x11 at all, so it
     * inherits whatever came before - and what comes before is JPEG_INIT, which
     * sets it to 0x00. That is the value measured to stop the capture path
     * working. Every stock table from 640x480 up writes 0x01 explicitly; this
     * one was the odd one out by accident rather than by intent.
     */
    { 0x11, 0x01 },
    /*
     * COM1 and 0x3D, pinned for the same reason CLKRC is.
     *
     * Neither the 320x240 table this was copied from nor JPEG_INIT writes
     * COM1 at all, so its value was whatever the last table to run had left
     * there - and that made the frame rate depend on history rather than on
     * configuration. Straight from reset this table gave 11.4 fps; after
     * switching to 640x480 and back it gave 17, with nothing else changed.
     * 640x480 is a UXGA table and writes both of these; coming back through it
     * was quietly configuring the SVGA mode.
     *
     * A setting that only appears after visiting another one is not a setting.
     * These are the values measured at 17 fps.
     *
     * Note that esp32-camera uses the other pair for SVGA - COM1 0x0A with
     * 0x3D 0x38, keeping 0x0F/0x34 for UXGA. Measured here, its SVGA pair is
     * the slow one. If the picture ever comes out short or cropped rather than
     * merely slower, that is the first thing to put back.
     */
    { 0x03, 0x0f }, { 0x3d, 0x34 },
    { 0x17, 0x11 }, { 0x18, 0x43 },
    { 0x19, 0x00 }, { 0x1a, 0x4b }, { 0x32, 0x09 }, { 0x4f, 0xca },
    { 0x50, 0xa8 }, { 0x5a, 0x23 }, { 0x6d, 0x00 }, { 0x39, 0x12 },
    { 0x35, 0xda }, { 0x22, 0x1a }, { 0x37, 0xc3 }, { 0x23, 0x00 },
    { 0x34, 0xc0 }, { 0x36, 0x1a }, { 0x06, 0x88 }, { 0x07, 0xc0 },
    { 0x0d, 0x87 }, { 0x0e, 0x41 }, { 0x4c, 0x00 },

    /* ---- DSP bank: 800x600 in, 800x600 out ---- */
    { 0xff, 0x00 },
    { 0xe0, 0x04 },                 /* hold the DSP blocks while reconfiguring */
    { 0xc0, 0x64 }, { 0xc1, 0x4b },           /* input  800 x 600 */
    { 0x86, 0x35 },
    /*
     * CTRLI, with the dividers cleared.
     *
     * 0x89 is bit7 plus a divider of one in each axis, which is a halving. Both
     * tables it was copied between are halvings: 1600x1200 -> 800x600 exactly,
     * and 800x600 -> 400x300 before the fine scaler takes it to 320x240. This
     * table asks for 800x600 out of 800x600, so there is nothing to halve, and
     * a /2 divider with a 1:1 output is a contradiction the DSP does not
     * resolve - it stops emitting VSYNC, which reads downstream as a dead
     * sensor.
     *
     * Bit 7 is kept because both working tables set it and nothing here knows
     * better. Only the divider fields are cleared.
     */
    { 0x50, 0x80 },
    { 0x51, 0xc8 }, { 0x52, 0x96 },           /* window 800 x 600 */
    { 0x53, 0x00 }, { 0x54, 0x00 }, { 0x55, 0x00 }, { 0x57, 0x00 },
    { 0x5a, 0xc8 }, { 0x5b, 0x96 }, { 0x5c, 0x00 },   /* output 800 x 600 */
    { 0xd3, 0x04 },                 /* as 320x240 inherits from JPEG_INIT */
    { 0xe0, 0x00 },

    { 0xff, 0xff },
};

/** The table that produces a given size, or NULL if we do not offer it. */
static const ov2640_reg_t *table_for_res(res_t res)
{
    switch (res) {
    case RES_160X120:   return OV2640_160x120_JPEG;
    case RES_176X144:   return OV2640_176x144_JPEG;
    case RES_320X240:   return OV2640_320x240_JPEG;
    case RES_352X288:   return OV2640_352x288_JPEG;
    case RES_640X480:   return OV2640_640x480_JPEG;
    case RES_800X600:   return OV2640_800x600_SVGA_JPEG;
    case RES_1024X768:  return OV2640_1024x768_JPEG;
    case RES_1280X1024: return OV2640_1280x1024_JPEG;
    case RES_1600X1200: return OV2640_1600x1200_JPEG;
    default:            return NULL;
    }
}

/*
 * Set the frame size by writing the whole table for it.
 *
 * On the Mega this was one register and a clock divider chosen from a tuning
 * table of our own. Here each size is a windowing and scaler configuration
 * spread across both banks, including the two clock registers - so the table
 * decides the clock, and we read back afterwards rather than imposing a value
 * the mode was not built for.
 */
int cam_set_framesize(res_t res)
{
    const ov2640_reg_t *table = table_for_res(res);

    if (!table) {
        printf("set_framesize: %d is not an OV2640 size\n", (int)res);
        return -1;
    }

    camera.res = res;
    ov2640_ctrl_reset();                /* the tables rewrite SDE, 0xC7, REG04 */

    /*
     * Only the small tables and our 800x600 one read the SVGA array. The
     * stock 640x480 and every size from 1024x768 up read the full UXGA array
     * and scale it down, so they run at the UXGA frame period whatever the
     * output size: 640x480 measured 5.9 fps against 17.1 at 800x600 on
     * 2026-10-22. Smaller is not faster here - say so where it is chosen.
     */
    if (res == RES_640X480 || res >= RES_1024X768) {
        printf("OV2640: %s reads the UXGA array - expect ~5.9 fps;"
               " 800x600 (native SVGA) runs ~17 fps\n",
               res == RES_640X480 ? "640x480" : "this size");
    }

    int ret = ov2640_write_table(table);

    /* The big modes rewrite the whole scaler; give them time before anything
     * reads back or starts a capture. */
    sleep_ms(res >= RES_1024X768 ? 100 : 30);

    /*
     * Our own PCLK divider on top of the table's, where one has been measured.
     *
     * The imported tables are left exactly as they came, because they are a
     * configuration known to produce pictures and editing them means disagreeing
     * with that. Overriding afterwards keeps the reference intact and keeps the
     * tuning somewhere a reader can see it was ours.
     */
    clk_tuning_t tune = tuned_clocks(res);
    if (tune.tuned) {
        cam_clk_div = tune.clkrc;
        cam_pll_div = tune.dvp_sp;
        ret += ov2640_apply_clock_div(10);
    }

    ov2640_read_clock_div();

    /*
     * In JPEG there are no lines to speak of - the sensor emits one continuous
     * entropy-coded stream and the frame ends at its EOI marker, not at a row
     * boundary. This number is therefore the size of the chunk the DMA collects
     * at a time, and nothing more. It shares a field with the RAW path, where
     * it really is a line.
     */
    camera.line_length = 512;

    return ret;
}

/*
 * JPEG is the only format this example configures.
 *
 * The three tables have to go in this order and the middle one looks wrong:
 * YUV422 is written on the way to JPEG because the DSP's compression block is
 * fed from the YUV path, so the path is set up first and compression turned on
 * after. Skipping it gives a sensor that answers SCCB and produces nothing a
 * decoder recognises.
 *
 * RGB565 and YUV422 output would each need their own table and a capture path
 * that counts rows instead of hunting markers. Neither is wired up, so they are
 * refused rather than silently treated as JPEG.
 */
int cam_set_pixformat(pixfmt_t pixformat)
{
    if (pixformat != PIXFORMAT_JPEG) {
        printf("set_pixformat: only JPEG is implemented for the OV2640 here\n");
        return -1;
    }

    camera.pixfmt = PIXFORMAT_JPEG;

    int ret = ov2640_write_table(OV2640_JPEG_INIT);
    ret += ov2640_write_table(OV2640_YUV422);
    ret += ov2640_write_table(OV2640_JPEG);

    /* COM10: HREF, VSYNC and PCLK polarity. 0x00 is the combination the DVP
     * capture program already expects - it waits on HREF high and samples on
     * the rising PCLK edge. If the picture turns out to be sheared or empty,
     * this register is the first thing to move, before the PIO program. */
    ov2640_bank(OV2640_BANK_SENSOR);
    ret += wr(OV2640_REG_COM10, 0x00);

    sleep_ms(50);
    return ret;
}

/*
 * Reset through COM7, and through the pin as well when one is fitted.
 *
 * The register path is enough on its own, which is why the board can come up
 * with only the XCLK wire soldered. It does depend on SCCB working, and SCCB
 * depends on XCLK - so this is not a way out of a dead clock.
 */
static int ov2640_reset(void)
{
    if (CAM_PIN_RESET >= 0) {
        gpio_put(CAM_PIN_RESET, 0);
        sleep_ms(5);
        gpio_put(CAM_PIN_RESET, 1);
        sleep_ms(5);
    }

    int ret = ov2640_bank(OV2640_BANK_SENSOR);
    ret += wr(OV2640_REG_COM7, 0x80);
    sleep_ms(100);

    return ret;
}



