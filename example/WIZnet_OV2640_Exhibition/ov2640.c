
#include "ov2640.h"
#include "ov2640_regs.h"
#ifdef USBCDC
#include "tusb.h"
#endif
#include "hardware/clocks.h"
#include "image_mega.pio.h"
#include <stdlib.h>
#include "string.h"

/*
 * The DVP wiring is inherited unchanged - it already maps one-to-one onto the
 * OV2640's twenty-pin connector, which is the reason this port is small.
 *
 * Two pins are new and both are soldered by hand, because the base board leaves
 * the corresponding connector pins unconnected:
 *
 *   XCLK  the master clock the sensor runs on. The Mega CCM had its own
 *         oscillator and this was -1. A bare OV2640 has nothing to run on until
 *         we give it something, and with no XCLK it does not answer SCCB either
 *         - which reads in a log as "I2C is broken".
 *
 *   RESET optional. The init sequence resets through COM7 instead, so a board
 *         with only the XCLK wire fitted will come up. Leave it at -1 until the
 *         wire exists; the code then skips the pin and uses the register.
 */
const int PIN_CAM_SIOC = 1;
const int PIN_CAM_SIOD = 0;
const int PIN_CAM_RESETB = -1;      /* GP28 once soldered; -1 = not fitted */
const int PIN_CAM_PWDN = 27;
const int PIN_CAM_XCLK = 26;        /* soldered to connector pin 8 */
const int PIN_CAM_VSYNC = 4;
const int PIN_CAM_Y2_PIO_BASE = 5;

/*
 * What to drive XCLK at.
 *
 * The OV2640 accepts 6 to 27 MHz. 20 MHz divides exactly out of the 200 MHz
 * system clock (ten counts, five of them high), so the PWM carries no
 * fractional error and no jitter beyond the PLL's own.
 *
 * If a scope shows a rounded triangle rather than a square, the limit is the
 * output pad and not the PWM: at the default 4 mA into the capacitance of a
 * flying wire, the edge takes about as long as the half period. The drive
 * strength is raised below for that reason; failing that, drop to 10 MHz, which
 * the sensor is equally happy with - frame rate falls with it, bring-up does not.
 */
#define XCLK_HZ             (20 * 1000 * 1000)

/* 4-byte aligned: the DMA writes 32-bit words straight into this buffer. */
uint8_t image_buff[1024 * 200] __attribute__((aligned(4)));

/*
 * Profiling counters for the last successful capture.
 *
 * ov2640_vsync_wait_us : time spent blocked waiting for the next frame to
 *                         start. Large values mean the frame period is
 *                         quantised by the sensor frame rate - the previous
 *                         frame finished, and we sat idle until VSYNC.
 * ov2640_readout_us    : time spent actually pulling pixel data out of the
 *                         sensor (PIO + DMA + memcpy per line).
 * ov2640_line_count    : lines captured, useful to sanity-check frame_length.
 */
volatile uint32_t ov2640_vsync_wait_us = 0;
volatile uint32_t ov2640_readout_us    = 0;
volatile uint32_t ov2640_line_count    = 0;

/*
 * Sensor clock dividers.
 *
 * These set how long the sensor takes to emit one frame, which measurements
 * showed is the dominant term in the frame period - far more than the time
 * spent moving the JPEG over the network. They used to be hard-coded to the
 * same pair for every resolution, which left the low and high modes running
 * much slower than 720p.
 *
 * Kept as variables so they can be swept at runtime (see ov2640_set_clock_div)
 * and so a tuned value survives a resolution change.
 */
volatile uint8_t ov2640_clk_div = 0x02;
volatile uint8_t ov2640_pll_div = 0x01;

/*
 * JPEG parser diagnostics for the last capture. The parser walks marker
 * segments to find the real end of the image; when it fails to, the capture
 * falls back to reading padding until VSYNC drops. These expose why.
 */
volatile uint32_t ov2640_parse_pos = 0;
volatile uint8_t  ov2640_in_scan   = 0;
volatile uint32_t ov2640_jpeg_end  = 0;

void spi_slave_init(spi_inst_t *spi);

/* Defined further down; needed by ov2640_recover(). */
int reset();
int set_pixformat(pixfmt_t pixformat);
int set_framesize(res_t res);

/*
 * SCCB, which is I2C with one difference that matters.
 *
 * A standard I2C read writes the register index, then issues a REPEATED START
 * and reads. SCCB does not do repeated starts: the index write is a complete
 * transaction ending in a STOP, and the read is a second one. That is what the
 * final `false` is doing in both calls below - it is the SDK's `nostop` flag,
 * and false means "do send the stop".
 *
 * Changing either to `true` makes this look more like textbook I2C and stops
 * the sensor answering. Worth knowing before the wiring gets blamed.
 */
uint8_t ov2640_reg_read(sensor_info_t *config, uint16_t reg)
{
    uint8_t index = (uint8_t)reg;
    uint8_t value = 0;

    i2c_write_blocking(config->sccb, config->sensor_address, &index, 1, false);
    i2c_read_blocking(config->sccb, config->sensor_address, &value, 1, false);

    return value;
}

/*
 * The caller's register is sixteen bits wide only because cam_controls.c is
 * still written against the Mega CCM's map. An OV2640 register is eight bits;
 * the high byte is dropped here, and cam_controls.c is blocked from reaching
 * this function at all until Step 7 remaps its table.
 */
uint8_t ov2640_reg_write(sensor_info_t *config, uint16_t reg, uint8_t value)
{
    uint8_t data[2] = { (uint8_t)reg, value };

    int ret = i2c_write_blocking(config->sccb, config->sensor_address, data, 2, false);
    return (ret == 2) ? 0 : 1;
}

/** Point the next accesses at the DSP block or at the sensor core. */
static int ov2640_bank(uint8_t bank)
{
    return ov2640_reg_write(&(camera.sensor), OV2640_REG_BANK, bank);
}

/*
 * Write one table, stopping at {0xff, 0xff}.
 *
 * Both bytes are checked. 0xff on its own is the bank-select register, which
 * every table writes several times; treating it as the end would truncate the
 * configuration at its first bank switch, and treating the terminator as a
 * write would select a bank that does not exist.
 */
static int ov2640_write_table(const ov2640_reg_t *t)
{
    int err = 0;

    for (; !(t->reg == OV2640_TABLE_END_REG && t->val == OV2640_TABLE_END_VAL); t++) {
        err += ov2640_reg_write(&(camera.sensor), t->reg, t->val);
    }
    return err;
}

/*
 * Read the sensor's identity.
 *
 * The two bytes are fixed in silicon, so this answers a different question from
 * "did the write succeed": it says the part is powered, clocked and talking.
 * Until it passes there is no point configuring anything.
 */
bool ov2640_probe(uint8_t *pid, uint8_t *ver)
{
    ov2640_bank(OV2640_BANK_SENSOR);

    uint8_t h = ov2640_reg_read(&(camera.sensor), OV2640_REG_PIDH);
    uint8_t l = ov2640_reg_read(&(camera.sensor), OV2640_REG_PIDL);

    if (pid) *pid = h;
    if (ver) *ver = l;

    return (h == OV2640_PID_HIGH && l == OV2640_PID_LOW);
}

/*
 * The master clock, which the Mega CCM supplied for itself and this sensor does
 * not. PWM rather than a GPOUT clock output: the four pins that can emit a
 * divided system clock are GP21 and GP23-25, and GP21 belongs to the W6300
 * while the rest are internal to the board.
 */
static void ov2640_xclk_init(int pin)
{
    if (pin < 0) {
        printf("XCLK: no pin configured - the sensor must have its own oscillator\n");
        return;
    }

    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t top    = (sys_hz / XCLK_HZ) - 1;   /* 200 MHz / 20 MHz -> 9 */

    gpio_set_function(pin, GPIO_FUNC_PWM);

    /*
     * Default drive is 4 mA with the slew rate limited, which at this frequency
     * spends most of a half period on the edge. Fast and 8 mA halves that. A
     * scope decides whether it was needed; it costs nothing to start here.
     */
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);

    uint slice = pwm_gpio_to_slice_num(pin);
    uint chan  = pwm_gpio_to_channel(pin);

    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, top);
    pwm_init(slice, &c, false);
    pwm_set_chan_level(slice, chan, (top + 1) / 2);
    pwm_set_enabled(slice, true);

    printf("XCLK: GP%d at %lu Hz (sys %lu Hz, wrap %lu)\n",
           pin, (unsigned long)(sys_hz / (top + 1)),
           (unsigned long)sys_hz, (unsigned long)top);
}



/**
 * Push the current clock dividers to the sensor.
 *
 * The Mega CCM had one controller register for each of these. The OV2640 keeps
 * them in different banks, and they are the same two knobs the resolution
 * tables themselves set:
 *
 *   CLKRC    (sensor bank 0x11)  divides the incoming XCLK for the sensor core
 *   R_DVP_SP (DSP bank 0xD3)     divides what comes out on PCLK
 *
 * Together they decide how fast pixels arrive, which is the main lever on frame
 * rate and the first thing to move when the picture tears.
 */
int ov2640_apply_clock_div(uint32_t settle_ms)
{
    int ret;

    ov2640_bank(OV2640_BANK_SENSOR);
    ret = ov2640_reg_write(&(camera.sensor), OV2640_REG_CLKRC, ov2640_clk_div);
    sleep_ms(settle_ms);

    ov2640_bank(OV2640_BANK_DSP);
    ret += ov2640_reg_write(&(camera.sensor), OV2640_REG_R_DVP_SP, ov2640_pll_div);
    sleep_ms(settle_ms);

    return ret;
}

/** Read back what the sensor is actually running at, after a table wrote it. */
static void ov2640_read_clock_div(void)
{
    ov2640_bank(OV2640_BANK_SENSOR);
    ov2640_clk_div = ov2640_reg_read(&(camera.sensor), OV2640_REG_CLKRC);

    ov2640_bank(OV2640_BANK_DSP);
    ov2640_pll_div = ov2640_reg_read(&(camera.sensor), OV2640_REG_R_DVP_SP);
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
int ov2640_set_clock_div(uint8_t clk_div, uint8_t pll_div)
{
    if (pll_div == 0) {
        printf("Sensor clock: rejected DVP_SP=0 (no PCLK would be emitted)\n");
        return -1;
    }

    ov2640_clk_div = clk_div;
    ov2640_pll_div = pll_div;
    printf("Sensor clock: CLKRC=0x%02X DVP_SP=0x%02X\n", clk_div, pll_div);
    return ov2640_apply_clock_div(10);
}

/**
 * Reset the sensor and restore the current format, resolution, and clock.
 * Recovers from a stalled clock without power-cycling the board.
 */
int ov2640_recover(void)
{
    int ret;

    printf("Sensor recovery: resetting...\n");
    ret = reset();
    sleep_ms(500);
    ret += set_pixformat(camera.frame.pixfmt);
    sleep_ms(200);
    ret += set_framesize(camera.frame.res);   /* re-applies clock dividers */
    sleep_ms(200);
    printf("Sensor recovery: done (res=%d, CLK_DIV=0x%02X, PLL_DIV=0x%02X)\n",
           (int)camera.frame.res, ov2640_clk_div, ov2640_pll_div);

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
int set_framesize(res_t res)
{
    const ov2640_reg_t *table = table_for_res(res);

    if (!table) {
        printf("set_framesize: %d is not an OV2640 size\n", (int)res);
        return -1;
    }

    camera.frame.res = res;

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
        ov2640_clk_div = tune.clkrc;
        ov2640_pll_div = tune.dvp_sp;
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
    camera.frame.line.length = 512;

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
int set_pixformat(pixfmt_t pixformat)
{
    if (pixformat != PIXFORMAT_JPEG) {
        printf("set_pixformat: only JPEG is implemented for the OV2640 here\n");
        return -1;
    }

    camera.frame.pixfmt = PIXFORMAT_JPEG;

    int ret = ov2640_write_table(OV2640_JPEG_INIT);
    ret += ov2640_write_table(OV2640_YUV422);
    ret += ov2640_write_table(OV2640_JPEG);

    /* COM10: HREF, VSYNC and PCLK polarity. 0x00 is the combination the DVP
     * capture program already expects - it waits on HREF high and samples on
     * the rising PCLK edge. If the picture turns out to be sheared or empty,
     * this register is the first thing to move, before the PIO program. */
    ov2640_bank(OV2640_BANK_SENSOR);
    ret += ov2640_reg_write(&(camera.sensor), OV2640_REG_COM10, 0x00);

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
int reset()
{
    if (PIN_CAM_RESETB >= 0) {
        gpio_put(PIN_CAM_RESETB, 0);
        sleep_ms(5);
        gpio_put(PIN_CAM_RESETB, 1);
        sleep_ms(5);
    }

    int ret = ov2640_bank(OV2640_BANK_SENSOR);
    ret += ov2640_reg_write(&(camera.sensor), OV2640_REG_COM7, 0x80);
    sleep_ms(100);

    return ret;
}



void ov2640_sensor_init(sensor_info_t *config){
    gpio_set_function(config->pin_sioc, GPIO_FUNC_I2C);
    gpio_set_function(config->pin_siod, GPIO_FUNC_I2C);

    gpio_pull_up(config->pin_sioc);
    gpio_pull_up(config->pin_siod);
    i2c_init(config->sccb, 200 * 1000);

    gpio_init(config->pin_vsync);
    gpio_set_dir(config->pin_vsync, GPIO_IN);
    gpio_pull_up(config->pin_vsync);

    /*
     * PWDN is active high, so low is awake.
     *
     * The inherited code called gpio_put() on this pin without ever claiming it
     * or setting a direction, which leaves it an input: the write went nowhere
     * and the sensor was held awake by whatever the board does by default. That
     * was survivable on the Mega. Here it decides whether the part responds at
     * all, so it is driven properly.
     */
    gpio_init(config->pin_pwdn);
    gpio_set_dir(config->pin_pwdn, GPIO_OUT);
    gpio_put(config->pin_pwdn, 0);

    if (config->pin_resetb >= 0) {
        gpio_init(config->pin_resetb);
        gpio_set_dir(config->pin_resetb, GPIO_OUT);
        gpio_put(config->pin_resetb, 1);        /* active low: high is running */
    }

    /* Before anything is said to the sensor - it has nothing to think with
     * until the clock arrives. */
    ov2640_xclk_init(config->pin_xclk);
    sleep_ms(10);

    uint8_t pid = 0, ver = 0;
    if (ov2640_probe(&pid, &ver)) {
        printf("OV2640 found: PID 0x%02X VER 0x%02X\n", pid, ver);
    } else {
        /*
         * Everything after this will fail, and it will fail in ways that look
         * like different problems - no frames, torn frames, a web server with
         * nothing to serve. Say plainly what did not happen, and where to look.
         */
        printf("OV2640 NOT FOUND on SCCB 0x%02X (read PID 0x%02X VER 0x%02X,"
               " expected 0x%02X 0x%02X)\n",
               config->sensor_address, pid, ver,
               OV2640_PID_HIGH, OV2640_PID_LOW);
        printf("  Check XCLK on GP%d, PWDN low on GP%d, and 3V3 before"
               " suspecting SIOC/SIOD.\n", config->pin_xclk, config->pin_pwdn);
    }

    printf("Resetting camera...\n");
    reset();

    printf("Setting JPEG format...\n");
    set_pixformat(PIXFORMAT_JPEG);

    printf("Setting initial resolution to 800x600...\n");
    set_framesize(RES_800X600);

    printf("Camera sensor initialization complete"
           " (CLKRC 0x%02X, DVP_SP 0x%02X)\n",
           ov2640_clk_div, ov2640_pll_div);
}



/*
 * Each line is DMA'd straight into its final place in image_buff.
 *
 * The PIO RX FIFO holds 8 words - 32 bytes. Whatever the state machine pushes
 * while the CPU is busy has to fit there or it is dropped silently, and a hole
 * in the middle of a JPEG shows up as a torn band across the picture.
 *
 * What matters is not the average byte rate over a frame but PCLK: during an
 * active line the bytes arrive back to back. Halving the sensor clock divider
 * doubles PCLK, which is why the low resolutions - the ones running the fastest
 * divider - were the ones tearing.
 *
 * So the per-line work is kept to arming the next transfer and scanning for the
 * end of the image. Copying the line separately would double that budget for no
 * reason, so the DMA lands in image_buff directly.
 */
static inline void ov2640_start_line_dma_to(uint8_t *dst) {
    dma_channel_set_trans_count(camera.pio_receiver.dma_channel,
                                camera.frame.line.length / 4, false);
    dma_channel_set_write_addr(camera.pio_receiver.dma_channel,
                               dst, true);
}



void ov2640_pio_init(pio_receiver_t *config){
    config->dma_channel = dma_claim_unused_channel(true);

    uint offset = pio_add_program(config->pio, &image_program);

    pio_sm_config c = image_program_get_default_config(offset);

    sm_config_set_in_pins(&c, config->pin_y2_pio_base);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_set_consecutive_pindirs(config->pio, config->pio_sm,
                                   config->pin_y2_pio_base, 10, false);

    uint gpio_func;
    if (config->pio == pio0) {
        gpio_func = GPIO_FUNC_PIO0;
    } else if (config->pio == pio1) {
        gpio_func = GPIO_FUNC_PIO1;
    } else {
        gpio_func = GPIO_FUNC_PIO0;
    }

    for (int i = 0; i < 10; i++) {
        gpio_set_function(config->pin_y2_pio_base + i, gpio_func);
    }

    pio_sm_init(config->pio, config->pio_sm, offset, &c);
    pio_sm_set_enabled(config->pio, config->pio_sm, true);

    config->dma_config = dma_channel_get_default_config(config->dma_channel);
    channel_config_set_read_increment(&config->dma_config, false);
    channel_config_set_write_increment(&config->dma_config, true);
    channel_config_set_dreq(&config->dma_config,
                            pio_get_dreq(config->pio, config->pio_sm, false));
    channel_config_set_transfer_data_size(&config->dma_config, DMA_SIZE_32);
    channel_config_set_high_priority(&config->dma_config, true);

    dma_channel_configure(
        config->dma_channel, &config->dma_config,
        camera.frame.line.buffer,
        &config->pio->rxf[config->pio_sm],
        camera.frame.line.length / 4,
        false
    );

    printf("[arducam pio_sm=%u, dma_ch=%d, pio_off=%u]\n",
           config->pio_sm, config->dma_channel, offset);
}
int ov2640_capture_frame() {
    uint32_t base_adress = 0;
    camera.frame.line.num = 0;
    camera.frame.frame_length = 0;
    bool jpeg_header_found = false;
    int retry_count = 0;
    const int max_retries = 3;
    uint32_t jpeg_end  = 0;     /* byte offset just past the JPEG EOI marker */
    uint32_t parse_pos = 2;     /* next byte the JPEG parser will look at    */
    bool     in_scan   = false; /* true once past SOS, i.e. in entropy data  */
    
    // Timeout constants
    const uint32_t VSYNC_TIMEOUT_US = 2000000;  // 2 seconds
    const uint32_t DMA_TIMEOUT_US = 500000;     // 500ms per line
    absolute_time_t timeout_start;
    absolute_time_t profile_start;              // whole-capture profiling
    absolute_time_t readout_start;

retry_capture:
    camera.frame.line.num = 0;
    camera.frame.frame_length = 0;
    jpeg_header_found = false;
    jpeg_end  = 0;
    parse_pos = 2;
    in_scan   = false;
    profile_start = get_absolute_time();

    // Wait for VSYNC to go LOW (with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(camera.sensor.pin_vsync) == true) {
        if (absolute_time_diff_us(timeout_start, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            printf("[ERR] VSYNC timeout waiting for LOW\n");
            return -2;
        }
        tight_loop_contents();
    }
    
    /*
     * Arm the first line while VSYNC is still low.
     *
     * The PIO state machine is free-running: it pushes a byte on every PCLK
     * edge that HREF qualifies. If the FIFO is drained and the DMA armed after
     * VSYNC rises, the sensor has already emitted the first bytes of the frame
     * and clearing the FIFO throws them away - which loses the JPEG SOI marker
     * and sends the capture into a retry.
     *
     * During vertical blanking HREF is low, so no data is produced and it is
     * safe to clear the FIFO and leave the DMA waiting on DREQ.
     */
    pio_sm_clear_fifos(camera.pio_receiver.pio, camera.pio_receiver.pio_sm);
    ov2640_start_line_dma_to(image_buff);

    // Wait for VSYNC to go HIGH (frame start, with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(camera.sensor.pin_vsync) == false) {
        if (absolute_time_diff_us(timeout_start, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            printf("[ERR] VSYNC timeout waiting for HIGH\n");
            dma_channel_abort(camera.pio_receiver.dma_channel);
            return -2;
        }
        tight_loop_contents();
    }

    /* Everything above was waiting for the sensor; pixel readout starts now. */
    readout_start = get_absolute_time();
    ov2640_vsync_wait_us =
        (uint32_t)absolute_time_diff_us(profile_start, readout_start);

    /*
     * Capture frame line by line, straight into image_buff.
     *
     * The FIFO is never cleared inside this loop: whatever the state machine
     * pushed between transfers is real image data and gets picked up by the
     * next one. Clearing it here would punch a silent hole in the middle of the
     * JPEG that the SOI check at line 0 cannot catch.
     */
    do {

        // Wait for DMA completion with timeout
        timeout_start = get_absolute_time();
        while (dma_channel_is_busy(camera.pio_receiver.dma_channel)) {
            if (absolute_time_diff_us(timeout_start, get_absolute_time()) > DMA_TIMEOUT_US) {
                printf("[ERR] DMA timeout on line %d\n", camera.frame.line.num);
                dma_channel_abort(camera.pio_receiver.dma_channel);
                return -3;
            }
            tight_loop_contents();
        }

        {
            uint32_t line_off = camera.frame.line.num * camera.frame.line.length;
            uint32_t scan_end = line_off + camera.frame.line.length;
            bool     room     = (scan_end + camera.frame.line.length) <= sizeof(image_buff);

            /*
             * Arm the next line before doing anything else. The state machine
             * keeps producing while the scan below runs and only 32 bytes fit
             * in the FIFO, so this has to come first.
             */
            if (room) {
                ov2640_start_line_dma_to(image_buff + scan_end);
            }

            camera.frame.line.num++;
            camera.frame.line.flag_end = 0;

            // Check for JPEG header in first few lines (JPEG mode only)
            if (line_off == 0) {
                camera.frame.line.flag_top = 1;
                if (image_buff[0] != 0xFF || image_buff[1] != 0xD8) {
                    retry_count++;
                    if (retry_count < max_retries) goto retry_capture;
                    else goto badframe;
                }
            } else {
                camera.frame.line.flag_top = 0;
            }

            /*
             * Walk the JPEG to find where it really ends.
             *
             * The sensor keeps driving data for the whole active frame period,
             * long after the image has finished, so without this the capture
             * reads padding until VSYNC drops - which at 1600x1200 and above
             * overruns image_buff and throws the frame away entirely.
             *
             * Scanning blindly for FF D9 is not safe: byte stuffing only
             * applies to entropy-coded data, so a quantisation or Huffman
             * table payload can contain FF D9 and would truncate the image
             * mid-header. Walk the marker segments by their length until SOS,
             * and only then look for the end marker.
             *
             * The parser is incremental - parse_pos persists across lines and
             * each byte is examined once. Inside the entropy data it hunts for
             * the next 0xFF with memchr rather than a byte loop, because this
             * runs between DMA transfers and the FIFO only buys 32 bytes of
             * slack.
             */
            while (!jpeg_end) {
                if (!in_scan) {
                    uint8_t  marker;
                    uint32_t seg_len;

                    if (parse_pos + 4 > scan_end) {
                        break;                      /* need more data */
                    }
                    if (image_buff[parse_pos] != 0xFF) {
                        parse_pos++;                /* resynchronise */
                        continue;
                    }
                    marker = image_buff[parse_pos + 1];

                    /* A run of FF bytes is legal padding before a marker. */
                    if (marker == 0xFF) {
                        parse_pos++;
                        continue;
                    }

                    /* Standalone markers carry no length field. */
                    if (marker == 0xD8 || marker == 0x01 ||
                        (marker >= 0xD0 && marker <= 0xD7)) {
                        parse_pos += 2;
                        continue;
                    }

                    seg_len = ((uint32_t)image_buff[parse_pos + 2] << 8) |
                               (uint32_t)image_buff[parse_pos + 3];
                    if (seg_len < 2) {
                        break;                      /* malformed - give up */
                    }
                    parse_pos += 2 + seg_len;
                    if (marker == 0xDA) {
                        in_scan = true;             /* entropy data follows */
                    }
                } else {
                    uint8_t *ff;

                    if (parse_pos + 1 >= scan_end) {
                        break;                      /* need more data */
                    }

                    ff = memchr(image_buff + parse_pos, 0xFF,
                                scan_end - parse_pos - 1);
                    if (ff == NULL) {
                        parse_pos = scan_end - 1;   /* keep the trailing byte */
                        break;
                    }

                    parse_pos = (uint32_t)(ff - image_buff);
                    if (image_buff[parse_pos + 1] == 0xD9) {
                        jpeg_end = parse_pos + 2;
                        break;
                    }
                    parse_pos += 2;                 /* FF 00 stuffing or RSTn */
                }
            }

            if (jpeg_end) {
                break;                  /* frame complete - stop reading padding */
            }

            /* No room was left for another line, so nothing more will arrive. */
            if(!room) {
                goto badframe;
            }
        }

    } while(gpio_get(camera.sensor.pin_vsync) == true);

    /* The last transfer was armed for a line that never arrived. */
    dma_channel_abort(camera.pio_receiver.dma_channel);
    
    // Frame capture completed
    camera.frame.line.flag_end = 1;

    camera.frame.frame_length =
        jpeg_end ? jpeg_end
                 : (camera.frame.line.num * camera.frame.line.length);

    ov2640_readout_us =
        (uint32_t)absolute_time_diff_us(readout_start, get_absolute_time());
    ov2640_line_count = camera.frame.line.num;
    ov2640_parse_pos  = parse_pos;
    ov2640_in_scan    = in_scan ? 1 : 0;
    ov2640_jpeg_end   = jpeg_end;

    /*
     * Without an EOI the image is incomplete - the tail was lost, or it did not
     * fit in image_buff. Report it rather than hand a truncated JPEG to the
     * caller: a decoder that rejects it makes the view blink, which is worse
     * than skipping the frame.
     */
    if (!jpeg_end) {
        return -4;
    }

    return 0;

badframe:
    /* Leave no transfer armed behind; the next capture arms its own. */
    dma_channel_abort(camera.pio_receiver.dma_channel);
    return -1;
}

void camera_init(){
    ov2640_sensor_init(&(camera.sensor));
    ov2640_pio_init(&(camera.pio_receiver));
}

void camera_handler(){
}

camera_t camera = {
    .frame.read_pixel_index = 0,
    .spi_controller = {
        .spi = spi1,
        .cap_sta = 0,
    },
    .sensor={
        .sccb = i2c0,
        .sccb_mode = I2C_MODE_8_8,
        .sensor_address = OV2640_SCCB_ADDR,
        .pin_sioc = PIN_CAM_SIOC,
        .pin_siod = PIN_CAM_SIOD,
        .pin_resetb = PIN_CAM_RESETB,
        .pin_pwdn   = PIN_CAM_PWDN,
        .pin_xclk = PIN_CAM_XCLK,
        .pin_vsync = PIN_CAM_VSYNC,
    },
    .pio_receiver = {
        .pin_y2_pio_base = PIN_CAM_Y2_PIO_BASE,
        .pio = pio0,
        .pio_sm = 0,
        .dma_channel = 0,
    },
    .init = camera_init,
    .set_frame_size = set_framesize,
    .set_pixel_format = set_pixformat,
    .get_frame =  ov2640_capture_frame,
    .mega_handler =camera_handler,

    .save_image_to_flash = 0,
};
