/**
 * @file    dvp_capture.c
 * @brief   8-bit DVP capture into a frame buffer: PIO + one DMA transfer +
 *          JPEG end detection.
 *
 * The PIO program (dvp_capture.pio) pushes a 32-bit word for every four bytes
 * the sensor clocks out while HREF is high. One DMA transfer, armed during
 * vertical blanking, moves those words into the whole buffer. The CPU never
 * re-arms anything while the frame is coming in: it only watches how far the
 * DMA has written and walks the JPEG behind it until the EOI marker.
 *
 * Why one transfer: the earlier capture took the frame in 512-byte DMA
 * transfers and re-armed the channel from the CPU after each one. The PIO
 * FIFO and shift register hold 36 bytes, so a re-arm that came late lost the
 * bytes after that - on the OV2640 about one frame in a thousand came out with
 * 6-8 bytes missing at offset 548 = 512 + 32 + 4, in the last Huffman table,
 * with the old and the new PIO program alike (measured 2026-10-28). With a
 * single transfer there is no re-arm, so no deadline for the CPU to miss.
 *
 * Written for this SDK on 2026-10-28, replacing the capture loop inherited
 * from the ArduCAM sample (see the provenance notes).
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

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
};

volatile uint32_t cam_vsync_wait_us = 0;
volatile uint32_t cam_readout_us    = 0;

/* How long VSYNC may take to change, and how long the DMA may go without
 * moving while VSYNC says the frame is still coming. */
#define VSYNC_TIMEOUT_US     2000000u
#define STALL_TIMEOUT_US      500000u
/* Attempts at a frame that does not start with SOI before giving up. */
#define SOI_ATTEMPTS               3

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
                          1, false);

    printf("[capture pio_sm=%u dma_ch=%u pio_off=%u D0=GP%d VSYNC=GP%d]\n",
           camera.pio_sm, camera.dma_channel, offset, camera.pin_d0, camera.pin_vsync);
}

/* ------------------------------------------------------------ JPEG walk -- */

/*
 * Finds the end of a JPEG as its bytes arrive.
 *
 * The sensor keeps clocking out filler after the image until VSYNC falls, so
 * the frame ends at the EOI marker (FF D9), not where the data stops. A plain
 * search for FF D9 is wrong: the header tables are raw bytes and may contain
 * that pair. So the header is walked segment by segment using each marker's
 * length, and only the entropy-coded data after SOS is searched - there the
 * encoder escapes every data FF as FF 00, so FF D9 can only be the real end.
 */
typedef struct {
    uint32_t pos;       /* next byte to look at */
    bool     in_scan;   /* past SOS: in entropy-coded data */
} jpeg_walk_t;

/* Advance over the first `have` bytes of b. Returns the length of the JPEG
 * (just past EOI) once found, 0 while more bytes are needed, or -1 if the
 * header is malformed. */
static int32_t jpeg_walk(jpeg_walk_t *w, const uint8_t *b, uint32_t have)
{
    while (!w->in_scan) {
        if (w->pos + 4 > have) {
            return 0;
        }
        if (b[w->pos] != 0xFF) {
            return -1;                      /* a segment must start here */
        }
        uint8_t m = b[w->pos + 1];
        if (m == 0xFF) {                    /* fill byte before a marker */
            w->pos += 1;
            continue;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            w->pos += 2;                    /* markers without a length */
            continue;
        }
        uint32_t len = ((uint32_t)b[w->pos + 2] << 8) | b[w->pos + 3];
        if (len < 2) {
            return -1;
        }
        w->pos += 2 + len;
        w->in_scan = (m == 0xDA);
    }

    while (w->pos + 2 <= have) {
        const uint8_t *ff = memchr(b + w->pos, 0xFF, have - w->pos - 1);
        if (ff == NULL) {
            w->pos = have - 1;              /* an FF may be the last byte */
            return 0;
        }
        w->pos = (uint32_t)(ff - b);
        if (b[w->pos + 1] == 0xD9) {
            return (int32_t)(w->pos + 2);
        }
        w->pos += 2;                        /* FF 00, or a restart marker */
    }
    return 0;
}

/* ------------------------------------------------------------- capture -- */

/* Bytes the DMA has written into buf so far. WRITE_ADDR moves on as each
 * write completes, so every byte below it is already in memory. */
static inline uint32_t dma_written(const uint8_t *buf)
{
    uint32_t wa = dma_channel_hw_addr(camera.dma_channel)->write_addr;
    __dmb();                                /* read the data after the address */
    return wa - (uint32_t)(uintptr_t)buf;
}

static bool wait_vsync(bool level, absolute_time_t *t0)
{
    *t0 = get_absolute_time();
    while (gpio_get(camera.pin_vsync) != level) {
        if (absolute_time_diff_us(*t0, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

int cam_capture_frame_into(uint8_t *buf, uint32_t cap)
{
    absolute_time_t t_start = get_absolute_time();
    absolute_time_t t;

    camera.frame_length = 0;
    cap &= ~3u;                             /* whole words only */

    for (int attempt = 0; attempt < SOI_ATTEMPTS; attempt++) {
        /* Arm during vertical blanking: HREF is low, nothing is pushed, so
         * emptying the FIFO loses nothing and the first byte of the frame
         * lands at buf[0]. */
        if (!wait_vsync(false, &t)) {
            printf("[ERR] VSYNC timeout waiting for LOW\n");
            return -2;
        }
        pio_sm_clear_fifos(camera.pio, camera.pio_sm);
        dma_channel_set_trans_count(camera.dma_channel, cap / 4, false);
        dma_channel_set_write_addr(camera.dma_channel, buf, true);

        if (!wait_vsync(true, &t)) {
            printf("[ERR] VSYNC timeout waiting for HIGH\n");
            dma_channel_abort(camera.dma_channel);
            return -2;
        }
        absolute_time_t t_read = t = get_absolute_time();
        cam_vsync_wait_us = (uint32_t)absolute_time_diff_us(t_start, t_read);

        jpeg_walk_t w = { .pos = 2, .in_scan = false };
        uint32_t seen = 0;
        int32_t  end  = 0;
        bool     soi_checked = false;

        for (;;) {
            bool frame_on = gpio_get(camera.pin_vsync);
            uint32_t have = dma_written(buf);

            if (have != seen) {
                seen = have;
                t = get_absolute_time();
            }
            if (!soi_checked && have >= 4) {
                if (buf[0] != 0xFF || buf[1] != 0xD8) {
                    break;                  /* not a frame start: try again */
                }
                soi_checked = true;
            }
            if (soi_checked) {
                end = jpeg_walk(&w, buf, have);
                if (end != 0) {
                    break;
                }
            }
            if (have >= cap) {
                /* Buffer full without EOI: the frame does not fit. Not a
                 * sensor fault - the caller must not reset the sensor over it
                 * (2026-10-26: UXGA on the LH module reset the sensor every
                 * ten frames and froze the stream). */
                dma_channel_abort(camera.dma_channel);
                return -5;
            }
            if (!frame_on) {
                /* VSYNC fell. `have` was read after the level, so it already
                 * holds everything pushed before the frame ended, apart from
                 * words still in the FIFO: one more pass once it drains. */
                while (!pio_sm_is_rx_fifo_empty(camera.pio, camera.pio_sm) &&
                       absolute_time_diff_us(t, get_absolute_time()) < 1000) {
                    tight_loop_contents();
                }
                have = dma_written(buf);
                if (soi_checked) {
                    end = jpeg_walk(&w, buf, have);
                }
                break;
            }
            if (absolute_time_diff_us(t, get_absolute_time()) > STALL_TIMEOUT_US) {
                printf("[ERR] DMA stalled at %lu bytes\n", (unsigned long)have);
                dma_channel_abort(camera.dma_channel);
                return -3;
            }
        }

        dma_channel_abort(camera.dma_channel);
        cam_readout_us = (uint32_t)absolute_time_diff_us(t_read, get_absolute_time());

        if (!soi_checked) {
            continue;
        }
        if (end > 0) {
            camera.frame_length = (uint32_t)end;
            return 0;
        }
        /* No EOI: the tail was lost or the header is broken. Report it rather
         * than hand on a truncated JPEG - a decoder that rejects it makes the
         * view blink, which is worse than skipping the frame. */
        return -4;
    }
    return -1;
}
