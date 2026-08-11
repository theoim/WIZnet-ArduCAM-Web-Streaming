
#include "arducam_mega.h"
#ifdef USBCDC
#include "tusb.h"
#endif
#include "image_mega.pio.h"
#include <stdlib.h>
#include "string.h"

const int PIN_CAM_SIOC = 1;
const int PIN_CAM_SIOD = 0;
const int PIN_CAM_RESETB = 1;
const int PIN_CAM_PWDN = 27;
const int PIN_CAM_XCLK = -1;
const int PIN_CAM_VSYNC = 4;
const int PIN_CAM_Y2_PIO_BASE = 5;

/* 4-byte aligned: the DMA writes 32-bit words straight into this buffer. */
uint8_t image_buff[1024 * 200] __attribute__((aligned(4)));

/*
 * Profiling counters for the last successful capture.
 *
 * arducam_vsync_wait_us : time spent blocked waiting for the next frame to
 *                         start. Large values mean the frame period is
 *                         quantised by the sensor frame rate - the previous
 *                         frame finished, and we sat idle until VSYNC.
 * arducam_readout_us    : time spent actually pulling pixel data out of the
 *                         sensor (PIO + DMA + memcpy per line).
 * arducam_line_count    : lines captured, useful to sanity-check frame_length.
 */
volatile uint32_t arducam_vsync_wait_us = 0;
volatile uint32_t arducam_readout_us    = 0;
volatile uint32_t arducam_line_count    = 0;

/*
 * Sensor clock dividers.
 *
 * These set how long the sensor takes to emit one frame, which measurements
 * showed is the dominant term in the frame period - far more than the time
 * spent moving the JPEG over the network. They used to be hard-coded to the
 * same pair for every resolution, which left the low and high modes running
 * much slower than 720p.
 *
 * Kept as variables so they can be swept at runtime (see arducam_set_clock_div)
 * and so a tuned value survives a resolution change.
 */
volatile uint8_t arducam_clk_div = 0x02;
volatile uint8_t arducam_pll_div = 0x01;

/*
 * JPEG parser diagnostics for the last capture. The parser walks marker
 * segments to find the real end of the image; when it fails to, the capture
 * falls back to reading padding until VSYNC drops. These expose why.
 */
volatile uint32_t arducam_parse_pos = 0;
volatile uint8_t  arducam_in_scan   = 0;
volatile uint32_t arducam_jpeg_end  = 0;

void spi_slave_init(spi_inst_t *spi);

/* Defined further down; needed by arducam_recover(). */
int reset();
int set_pixformat(pixfmt_t pixformat);
int set_framesize(res_t res);

uint8_t arducam_reg_read(sensor_info_t *config, uint16_t reg)
{
    uint8_t data[2];
    uint8_t length;
    switch (config->sccb_mode)
    {
    case I2C_MODE_16_8:
        data[0] = (uint8_t)(reg >> 8) & 0xFF;
        data[1] = (uint8_t)(reg)&0xFF;
        length = 2;
        break;
    }

    i2c_write_blocking(config->sccb, config->sensor_address, data, length, false);
    
    uint8_t value;
    i2c_read_blocking(config->sccb, config->sensor_address, &value, 1, false);

    return value;
}

uint8_t arducam_reg_write(sensor_info_t *config, uint16_t reg, uint8_t value)
{
    uint8_t data[3];
    uint8_t length = 0;
    switch (config->sccb_mode)
    {
    case I2C_MODE_16_8:
        data[0] = (uint8_t)(reg >> 8) & 0xFF;
        data[1] = (uint8_t)(reg)&0xFF;
        data[2] = value;
        length = 3;
        break;
    }
    int ret = i2c_write_blocking(config->sccb, config->sensor_address, data, length, false);
    return (ret == length) ? 0 : 1;
}



/**
 * Push the current clock dividers to the sensor.
 * Separated out so the resolution path and the runtime tuning path write the
 * same two registers the same way.
 */
int arducam_apply_clock_div(uint32_t settle_ms)
{
    int ret;

    ret = arducam_reg_write(&(arducam_mega.sensor), SYSTEM_CLK_DIV_REG, arducam_clk_div);
    sleep_ms(settle_ms);
    ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_PLL_DIV_REG, arducam_pll_div);
    sleep_ms(settle_ms);

    return ret;
}

/**
 * Change the clock dividers at runtime and re-apply them immediately.
 *
 * A divider of 0 stops the sensor clock: VSYNC stays high, no frame is ever
 * emitted, and writing a valid divider afterwards does not restart it - the
 * sensor has to be reset. Reject 0 rather than let a sweep walk into a state
 * that needs a power cycle.
 */
int arducam_set_clock_div(uint8_t clk_div, uint8_t pll_div)
{
    if (clk_div == 0 || pll_div == 0) {
        printf("Sensor clock: rejected CLK_DIV=0x%02X PLL_DIV=0x%02X"
               " (0 stops the clock)\n", clk_div, pll_div);
        return -1;
    }

    arducam_clk_div = clk_div;
    arducam_pll_div = pll_div;
    printf("Sensor clock: CLK_DIV=0x%02X PLL_DIV=0x%02X\n", clk_div, pll_div);
    return arducam_apply_clock_div(10);
}

/**
 * Reset the sensor and restore the current format, resolution, and clock.
 * Recovers from a stalled clock without power-cycling the board.
 */
int arducam_recover(void)
{
    int ret;

    printf("Sensor recovery: resetting...\n");
    ret = reset();
    sleep_ms(500);
    ret += set_pixformat(arducam_mega.frame.pixfmt);
    sleep_ms(200);
    ret += set_framesize(arducam_mega.frame.res);   /* re-applies clock dividers */
    sleep_ms(200);
    printf("Sensor recovery: done (res=%d, CLK_DIV=0x%02X, PLL_DIV=0x%02X)\n",
           (int)arducam_mega.frame.res, arducam_clk_div, arducam_pll_div);

    return ret;
}

/*
 * Per-resolution sensor clock divider.
 *
 * The divider scales readout time directly - halving it halves the time - so a
 * single shared value leaves most modes running at half speed. What stops it
 * from being 1 everywhere is that the faster pixel clock corrupts some modes.
 * Measured on this module:
 *
 *   320x240    1 halves readout to 29 ms but tears the picture: bands of the
 *              image shift sideways where bytes went missing. 2 is clean.
 *   640x480    1 is clean here - same 29 ms readout, no tearing.
 *   1280x720   1 gives 22 ms against 44 ms at 2, and stays clean. Roughly
 *              triples the frame rate.
 *   1600x1200  2 for now. 1 was unusable before the capture loop stopped
 *   1920x1080  copying every line, and has not been retested since.
 */
static uint8_t clk_div_for_res(res_t res)
{
    switch (res) {
    case RES_640X480:
    case RES_1280X720:
        return 0x01;
    case RES_320X240:
    case RES_1600X1200:
    case RES_1920X1080:
    default:
        return 0x02;
    }
}

int set_framesize(res_t res)
{
    int      ret = 0;
    uint8_t  res_reg;
    uint32_t settle_ms;

    arducam_mega.frame.res = res;
    switch (res){
        case RES_320X240:   res_reg = 0x01; settle_ms = 10;  break;
        case RES_640X480:   res_reg = 0x02; settle_ms = 10;  break;
        case RES_1280X720:  res_reg = 0x03; settle_ms = 10;  break;
        case RES_1600X1200: res_reg = 0x04; settle_ms = 100; break;
        case RES_1920X1080: res_reg = 0x05; settle_ms = 100; break;
        default:
            arducam_mega.frame.res = RES_320X240;
            res_reg = 0x01;
            settle_ms = 10;
            ret = -1;
        break;
    }

    /*
     * Adopt the tuned divider for the new mode. A value set by hand through
     * arducam_set_clock_div() is intentionally overwritten here - it was chosen
     * for the resolution that was active at the time.
     */
    arducam_clk_div = clk_div_for_res(arducam_mega.frame.res);

    ret += arducam_reg_write(&(arducam_mega.sensor), RESOLUTION_REG, res_reg);

    /*
     * Give the sensor time to finish switching modes before touching the clock.
     * With only the short settle the divider write was silently ignored at the
     * lower resolutions, leaving whatever value happened to be programmed - so
     * the per-resolution table above had no effect there.
     */
    sleep_ms(settle_ms < 100 ? 100 : settle_ms);
    ret += arducam_apply_clock_div(settle_ms);

    if(arducam_mega.frame.pixfmt == PIXFORMAT_JPEG){
        arducam_mega.frame.line.length = 512;
    }else{
        switch(arducam_mega.frame.res){
            case RES_320X240:
                arducam_mega.frame.line.length = 320*2;
            break;
            case RES_640X480:
                arducam_mega.frame.line.length = 640*2;
            break;
            case RES_1280X720:
                arducam_mega.frame.line.length = 1280*2;
            break;
            case RES_1600X1200:
                arducam_mega.frame.line.length = 1600*2;
            break;
            case RES_1920X1080:
                arducam_mega.frame.line.length = 1920*2;
            break;
        }
    }
    return ret;
}

int set_pixformat(pixfmt_t pixformat)
{
    int ret = 0;
    arducam_mega.frame.pixfmt = pixformat;
    switch (pixformat) {
    case PIXFORMAT_JPEG:
        ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x01);
        break;
    case PIXFORMAT_RGB565:
         ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x02);
        break;
    case PIXFORMAT_YUV422:
        ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x03);
        break;
    default:
        ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x01);
        arducam_mega.frame.pixfmt = PIXFORMAT_JPEG;
        break;
    }

    return ret;
}

int reset()
{
    int ret;
    ret = arducam_reg_write(&(arducam_mega.sensor), CAMERA_RST_REG, 0x00);
    ret += arducam_reg_write(&(arducam_mega.sensor), CAMERA_RST_REG, 0x01);
    sleep_ms(1000);
    return ret;
}



void arducam_sensor_init(sensor_info_t *config){
    gpio_set_function(config->pin_sioc, GPIO_FUNC_I2C);
    gpio_set_function(config->pin_siod, GPIO_FUNC_I2C);
    
    gpio_pull_up(config->pin_sioc);
    gpio_pull_up(config->pin_siod);
    i2c_init(config->sccb, 200 * 1000);

    gpio_init(config->pin_vsync);
    gpio_set_dir(config->pin_vsync, GPIO_IN);
    gpio_pull_up(config->pin_vsync);
    
    gpio_put(config->pin_pwdn, 0);
    sleep_ms(200);

    printf("Resetting camera...\n");
    reset();
    sleep_ms(500);
    
    printf("Setting JPEG format...\n");
    set_pixformat(PIXFORMAT_JPEG);
    sleep_ms(200);
    
    printf("Setting initial resolution to 320x240...\n");
    set_framesize(RES_320X240);
    sleep_ms(200);

    printf("Camera sensor initialization complete\n");
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
static inline void arducam_start_line_dma_to(uint8_t *dst) {
    dma_channel_set_trans_count(arducam_mega.pio_receiver.dma_channel,
                                arducam_mega.frame.line.length / 4, false);
    dma_channel_set_write_addr(arducam_mega.pio_receiver.dma_channel,
                               dst, true);
}



void arducam_pio_init(pio_receiver_t *config){
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
        arducam_mega.frame.line.buffer,
        &config->pio->rxf[config->pio_sm],
        arducam_mega.frame.line.length / 4,
        false
    );

    printf("[arducam pio_sm=%u, dma_ch=%d, pio_off=%u]\n",
           config->pio_sm, config->dma_channel, offset);
}
int arducam_capture_frame() {
    uint32_t base_adress = 0;
    arducam_mega.frame.line.num = 0;
    arducam_mega.frame.frame_length = 0;
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
    arducam_mega.frame.line.num = 0;
    arducam_mega.frame.frame_length = 0;
    jpeg_header_found = false;
    jpeg_end  = 0;
    parse_pos = 2;
    in_scan   = false;
    profile_start = get_absolute_time();

    // Wait for VSYNC to go LOW (with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(arducam_mega.sensor.pin_vsync) == true) {
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
    pio_sm_clear_fifos(arducam_mega.pio_receiver.pio, arducam_mega.pio_receiver.pio_sm);
    arducam_start_line_dma_to(image_buff);

    // Wait for VSYNC to go HIGH (frame start, with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(arducam_mega.sensor.pin_vsync) == false) {
        if (absolute_time_diff_us(timeout_start, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            printf("[ERR] VSYNC timeout waiting for HIGH\n");
            dma_channel_abort(arducam_mega.pio_receiver.dma_channel);
            return -2;
        }
        tight_loop_contents();
    }

    /* Everything above was waiting for the sensor; pixel readout starts now. */
    readout_start = get_absolute_time();
    arducam_vsync_wait_us =
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
        while (dma_channel_is_busy(arducam_mega.pio_receiver.dma_channel)) {
            if (absolute_time_diff_us(timeout_start, get_absolute_time()) > DMA_TIMEOUT_US) {
                printf("[ERR] DMA timeout on line %d\n", arducam_mega.frame.line.num);
                dma_channel_abort(arducam_mega.pio_receiver.dma_channel);
                return -3;
            }
            tight_loop_contents();
        }

        {
            uint32_t line_off = arducam_mega.frame.line.num * arducam_mega.frame.line.length;
            uint32_t scan_end = line_off + arducam_mega.frame.line.length;
            bool     room     = (scan_end + arducam_mega.frame.line.length) <= sizeof(image_buff);

            /*
             * Arm the next line before doing anything else. The state machine
             * keeps producing while the scan below runs and only 32 bytes fit
             * in the FIFO, so this has to come first.
             */
            if (room) {
                arducam_start_line_dma_to(image_buff + scan_end);
            }

            arducam_mega.frame.line.num++;
            arducam_mega.frame.line.flag_end = 0;

            // Check for JPEG header in first few lines (JPEG mode only)
            if (line_off == 0) {
                arducam_mega.frame.line.flag_top = 1;
                if (image_buff[0] != 0xFF || image_buff[1] != 0xD8) {
                    retry_count++;
                    if (retry_count < max_retries) goto retry_capture;
                    else goto badframe;
                }
            } else {
                arducam_mega.frame.line.flag_top = 0;
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

    } while(gpio_get(arducam_mega.sensor.pin_vsync) == true);

    /* The last transfer was armed for a line that never arrived. */
    dma_channel_abort(arducam_mega.pio_receiver.dma_channel);
    
    // Frame capture completed
    arducam_mega.frame.line.flag_end = 1;

    arducam_mega.frame.frame_length =
        jpeg_end ? jpeg_end
                 : (arducam_mega.frame.line.num * arducam_mega.frame.line.length);

    arducam_readout_us =
        (uint32_t)absolute_time_diff_us(readout_start, get_absolute_time());
    arducam_line_count = arducam_mega.frame.line.num;
    arducam_parse_pos  = parse_pos;
    arducam_in_scan    = in_scan ? 1 : 0;
    arducam_jpeg_end   = jpeg_end;

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
    dma_channel_abort(arducam_mega.pio_receiver.dma_channel);
    return -1;
}

void arducam_mega_init(){
    arducam_sensor_init(&(arducam_mega.sensor));
    arducam_pio_init(&(arducam_mega.pio_receiver));
}

void arducam_mega_handler(){
}

arducam_mega_t arducam_mega = {
    .frame.read_pixel_index = 0,
    .spi_controller = {
        .spi = spi1,
        .cap_sta = 0,
    },
    .sensor={
        .sccb = i2c0,
        .sccb_mode = I2C_MODE_16_8,
        .sensor_address = 0x1F,
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
    .init = arducam_mega_init,
    .set_frame_size = set_framesize,
    .set_pixel_format = set_pixformat,
    .get_frame =  arducam_capture_frame,
    .mega_handler =arducam_mega_handler,

    .save_image_to_flash = 0,
};
