/**
 * @file    dvp_capture.c
 * @brief   8-bit DVP capture into image_buff: PIO + DMA + JPEG end detection.
 *
 * Moved here unchanged from the OV2640 exhibition driver (ov2640.c), which had
 * it unchanged from arducam_mega.c - the two were identical apart from the name
 * of the global object. The comments inside cam_capture_frame() are the
 * originals and still describe it exactly; only names changed:
 *
 *   camera.frame.line.length  -> camera.line_length
 *   camera.frame.line.num     -> camera.line_num
 *   camera.frame.frame_length -> camera.frame_length
 *   camera.pio_receiver.*     -> camera.pio / pio_sm / dma_channel
 *   ov2640_*_us, ov2640_*     -> cam_*
 *
 * Dropped: line.flag_top / flag_end, which nothing read, and line.buffer, a
 * 5 KB array the DMA was only ever pointed at during configuration.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "dvp_capture.h"
#include "dvp_capture.pio.h"
#include "stream_config.h"        /* STREAM_PIPELINE */
#include CAM_MODULE_HEADER

#if !STREAM_PIPELINE
/* 4-byte aligned: the DMA writes 32-bit words straight into this buffer. In the
 * pipelined build the two frame buffers live in cam_pipeline.c instead. */
uint8_t image_buff[CAM_IMAGE_BUFF_SIZE] __attribute__((aligned(4)));

int cam_capture_frame(void)
{
    return cam_capture_frame_into(image_buff, CAM_IMAGE_BUFF_SIZE);
}
#endif

/* Where the DMA points between captures. Every capture re-arms it into the
 * real buffer before the first byte can arrive. */
static uint32_t s_dma_parking __attribute__((aligned(4)));

cam_capture_t camera = {
    .pin_vsync   = CAM_PIN_VSYNC,
    .pin_d0      = CAM_PIN_D0,
    .pio         = pio0,
    .pio_sm      = 0,
    .pixfmt      = PIXFORMAT_JPEG,
    .line_length = 512,
};

volatile uint32_t cam_vsync_wait_us = 0;
volatile uint32_t cam_readout_us    = 0;
volatile uint32_t cam_line_count    = 0;
volatile uint32_t cam_parse_pos     = 0;
volatile uint8_t  cam_in_scan       = 0;
volatile uint32_t cam_jpeg_end      = 0;

/*
 * Each line is DMA'd straight into its final place in image_buff.
 *
 * The PIO RX FIFO holds 8 words - 32 bytes. Whatever the state machine pushes
 * while the CPU is busy has to fit there or it is dropped silently, and a hole
 * in the middle of a JPEG shows up as a torn band across the picture. So the
 * per-line work is kept to arming the next transfer and scanning for the end
 * of the image.
 */
static inline void start_line_dma_to(uint8_t *dst)
{
    dma_channel_set_trans_count(camera.dma_channel, camera.line_length / 4, false);
    dma_channel_set_write_addr(camera.dma_channel, dst, true);
}

void cam_capture_init(void)
{
    gpio_init(camera.pin_vsync);
    gpio_set_dir(camera.pin_vsync, GPIO_IN);
    gpio_pull_up(camera.pin_vsync);

    camera.dma_channel = dma_claim_unused_channel(true);

    uint offset = pio_add_program(camera.pio, &dvp_capture_program);
    pio_sm_config c = dvp_capture_program_get_default_config(offset);

    sm_config_set_in_pins(&c, camera.pin_d0);
    sm_config_set_jmp_pin(&c, camera.pin_d0 + dvp_capture_HREF_OFFSET);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    /* D0..D7, PCLK, HREF: ten consecutive inputs. */
    pio_sm_set_consecutive_pindirs(camera.pio, camera.pio_sm, camera.pin_d0, 10, false);

    uint gpio_func = (camera.pio == pio1) ? GPIO_FUNC_PIO1 : GPIO_FUNC_PIO0;
    for (int i = 0; i < 10; i++) {
        gpio_set_function(camera.pin_d0 + i, gpio_func);
    }

    pio_sm_init(camera.pio, camera.pio_sm, offset, &c);
    pio_sm_set_enabled(camera.pio, camera.pio_sm, true);

    camera.dma_config = dma_channel_get_default_config(camera.dma_channel);
    channel_config_set_read_increment(&camera.dma_config, false);
    channel_config_set_write_increment(&camera.dma_config, true);
    channel_config_set_dreq(&camera.dma_config,
                            pio_get_dreq(camera.pio, camera.pio_sm, false));
    channel_config_set_transfer_data_size(&camera.dma_config, DMA_SIZE_32);
    channel_config_set_high_priority(&camera.dma_config, true);

    dma_channel_configure(camera.dma_channel, &camera.dma_config,
                          &s_dma_parking, &camera.pio->rxf[camera.pio_sm],
                          camera.line_length / 4, false);

    printf("[capture pio_sm=%u dma_ch=%u pio_off=%u D0=GP%d VSYNC=GP%d]\n",
           camera.pio_sm, camera.dma_channel, offset, camera.pin_d0, camera.pin_vsync);
}

int cam_capture_frame_into(uint8_t *buf, uint32_t cap) {
    camera.line_num = 0;
    camera.frame_length = 0;
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
    camera.line_num = 0;
    camera.frame_length = 0;
    jpeg_end  = 0;
    parse_pos = 2;
    in_scan   = false;
    profile_start = get_absolute_time();

    // Wait for VSYNC to go LOW (with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(camera.pin_vsync) == true) {
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
    pio_sm_clear_fifos(camera.pio, camera.pio_sm);
    start_line_dma_to(buf);

    // Wait for VSYNC to go HIGH (frame start, with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(camera.pin_vsync) == false) {
        if (absolute_time_diff_us(timeout_start, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            printf("[ERR] VSYNC timeout waiting for HIGH\n");
            dma_channel_abort(camera.dma_channel);
            return -2;
        }
        tight_loop_contents();
    }

    /* Everything above was waiting for the sensor; pixel readout starts now. */
    readout_start = get_absolute_time();
    cam_vsync_wait_us =
        (uint32_t)absolute_time_diff_us(profile_start, readout_start);

    /*
     * Capture frame line by line, straight into buf.
     *
     * The FIFO is never cleared inside this loop: whatever the state machine
     * pushed between transfers is real image data and gets picked up by the
     * next one. Clearing it here would punch a silent hole in the middle of the
     * JPEG that the SOI check at line 0 cannot catch.
     */
    do {

        // Wait for DMA completion with timeout
        timeout_start = get_absolute_time();
        while (dma_channel_is_busy(camera.dma_channel)) {
            if (absolute_time_diff_us(timeout_start, get_absolute_time()) > DMA_TIMEOUT_US) {
                printf("[ERR] DMA timeout on line %d\n", camera.line_num);
                dma_channel_abort(camera.dma_channel);
                return -3;
            }
            tight_loop_contents();
        }

        {
            uint32_t line_off = camera.line_num * camera.line_length;
            uint32_t scan_end = line_off + camera.line_length;
            bool     room     = (scan_end + camera.line_length) <= cap;

            /*
             * Arm the next line before doing anything else. The state machine
             * keeps producing while the scan below runs and only 32 bytes fit
             * in the FIFO, so this has to come first.
             */
            if (room) {
                start_line_dma_to(buf + scan_end);
            }

            camera.line_num++;

            // Check for JPEG header in first few lines (JPEG mode only)
            if (line_off == 0) {
                if (buf[0] != 0xFF || buf[1] != 0xD8) {
                    retry_count++;
                    if (retry_count < max_retries) goto retry_capture;
                    else goto badframe;
                }
            }

            /*
             * Walk the JPEG to find where it really ends.
             *
             * The sensor keeps driving data for the whole active frame period,
             * long after the image has finished, so without this the capture
             * reads padding until VSYNC drops - which at 1600x1200 and above
             * overruns buf and throws the frame away entirely.
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
                    if (buf[parse_pos] != 0xFF) {
                        parse_pos++;                /* resynchronise */
                        continue;
                    }
                    marker = buf[parse_pos + 1];

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

                    seg_len = ((uint32_t)buf[parse_pos + 2] << 8) |
                               (uint32_t)buf[parse_pos + 3];
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

                    ff = memchr(buf + parse_pos, 0xFF,
                                scan_end - parse_pos - 1);
                    if (ff == NULL) {
                        parse_pos = scan_end - 1;   /* keep the trailing byte */
                        break;
                    }

                    parse_pos = (uint32_t)(ff - buf);
                    if (buf[parse_pos + 1] == 0xD9) {
                        jpeg_end = parse_pos + 2;
                        break;
                    }
                    parse_pos += 2;                 /* FF 00 stuffing or RSTn */
                }
            }

            if (jpeg_end) {
                break;                  /* frame complete - stop reading padding */
            }

            /* No room was left for another line: the frame is larger than
             * the buffer. Not a sensor fault - the caller must not reset the
             * sensor over it (2026-10-26: UXGA on the LH module reset the
             * sensor every ten frames and froze the stream). */
            if(!room) {
                dma_channel_abort(camera.dma_channel);
                return -5;
            }
        }

    } while(gpio_get(camera.pin_vsync) == true);

    /* The last transfer was armed for a line that never arrived. */
    dma_channel_abort(camera.dma_channel);
    
    // Frame capture completed

    camera.frame_length =
        jpeg_end ? jpeg_end
                 : (camera.line_num * camera.line_length);

    cam_readout_us =
        (uint32_t)absolute_time_diff_us(readout_start, get_absolute_time());
    cam_line_count = camera.line_num;
    cam_parse_pos  = parse_pos;
    cam_in_scan    = in_scan ? 1 : 0;
    cam_jpeg_end   = jpeg_end;

    /*
     * Without an EOI the image is incomplete - the tail was lost, or it did not
     * fit in buf. Report it rather than hand a truncated JPEG to the
     * caller: a decoder that rejects it makes the view blink, which is worse
     * than skipping the frame.
     */
    if (!jpeg_end) {
        return -4;
    }

    return 0;

badframe:
    /* Leave no transfer armed behind; the next capture arms its own. */
    dma_channel_abort(camera.dma_channel);
    return -1;
}
