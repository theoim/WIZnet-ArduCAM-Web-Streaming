/**
 * @file    dvp_capture.h
 * @brief   8-bit DVP capture into image_buff: PIO + DMA + JPEG end detection.
 *
 * Sensor independent. It was the same code in arducam_mega.c and ov2640.c,
 * byte for byte apart from the object name, and lives here once now.
 *
 * Pins come from the module header: D0..D7 on CAM_PIN_D0..+7, PCLK on +8, HREF
 * on +9 (the PIO program waits on those two), VSYNC on CAM_PIN_VSYNC.
 */

#ifndef DVP_CAPTURE_H
#define DVP_CAPTURE_H

#include <stdint.h>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "cam_driver.h"

typedef struct {
    /* hardware */
    int  pin_vsync;
    int  pin_d0;
    PIO  pio;
    uint pio_sm;
    uint dma_channel;
    dma_channel_config dma_config;

    /* current mode - written by the sensor driver */
    pixfmt_t pixfmt;
    res_t    res;
    uint32_t line_length;      /* DMA chunk size; in JPEG not a real row */

    /* last capture */
    uint32_t line_num;
    uint32_t frame_length;
} cam_capture_t;

extern cam_capture_t camera;

void cam_capture_init(void);

/* Capture one frame into buf (4-byte aligned, cap bytes).
 * 0 = a complete JPEG (SOI..EOI), camera.frame_length bytes.
 * -1 bad frame, -2 VSYNC timeout, -3 DMA timeout, -4 no EOI (truncated or
 * larger than cap). Blocks for VSYNC: in the pipelined build only core 1 calls it. */
int  cam_capture_frame_into(uint8_t *buf, uint32_t cap);

/* Sequential build only (STREAM_PIPELINE 0): one 200 KB buffer. */
#define CAM_IMAGE_BUFF_SIZE  (1024 * 200)
extern uint8_t image_buff[];
int  cam_capture_frame(void);

/* Profiling of the last successful capture. */
extern volatile uint32_t cam_vsync_wait_us;   /* blocked waiting for frame start */
extern volatile uint32_t cam_readout_us;      /* pulling pixel data              */
extern volatile uint32_t cam_line_count;
extern volatile uint32_t cam_parse_pos;
extern volatile uint8_t  cam_in_scan;
extern volatile uint32_t cam_jpeg_end;

#endif /* DVP_CAPTURE_H */
