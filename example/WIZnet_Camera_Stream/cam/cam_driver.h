/**
 * @file    cam_driver.h
 * @brief   What every sensor driver provides. One driver is linked per image.
 *
 * The streaming code calls only these. Which driver answers is decided at
 * configure time (-DCAMERA_MODULE=..., which selects cam/<sensor>/), so there is
 * no table of function pointers and no runtime dispatch: each driver defines
 * the same names.
 *
 * Resolutions are one shared list. A driver supports a subset and refuses the
 * rest with a negative return - cam_sensor_supports() says which in advance.
 * The values are this library's own numbering, not register codes; each driver
 * maps them to whatever its sensor wants.
 */

#ifndef CAM_DRIVER_H
#define CAM_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "cam_ctrl.h"

typedef enum {
    PIXFORMAT_JPEG   = 1,
    PIXFORMAT_RGB565 = 2,
    PIXFORMAT_YUV422 = 3,
} pixfmt_t;

typedef enum {
    RES_160X120 = 1,
    RES_176X144,
    RES_320X240,
    RES_352X288,
    RES_640X480,
    RES_800X600,
    RES_1024X768,
    RES_1280X720,
    RES_1280X1024,
    RES_1600X1200,
    RES_1920X1080,
    RES_COUNT
} res_t;

typedef struct {
    const char *name;        /* sensor or module, for logs and /api/status */
    bool        has_jpeg;    /* frames come out as JPEG; false = raw rows   */
    bool        mono;
    res_t       default_res; /* the measured, recommended starting size     */
} cam_caps_t;

extern const cam_caps_t cam_caps;

/* Power, clock, SCCB, identify the part, reset, JPEG, default size.
 * Prints what it found; a missing or wrong part is reported, not fatal. */
void cam_sensor_init(void);

/* True when the chip ID matched what this driver drives. Valid after init. */
bool cam_sensor_found(void);

bool cam_sensor_supports(res_t res);
int  cam_set_framesize(res_t res);
int  cam_set_pixformat(pixfmt_t fmt);

/* Sensor clock dividers - the main lever on frame rate. Their meaning is the
 * driver's (OV2640: CLKRC / R_DVP_SP; Arducam: SYSTEM_CLK_DIV / SYSTEM_PLL_DIV). */
extern volatile uint8_t cam_clk_div;
extern volatile uint8_t cam_pll_div;
int  cam_set_clock_div(uint8_t clk_div, uint8_t pll_div);

/* Reset the sensor and restore format, size and clock without a power cycle. */
int  cam_recover(void);

/* Image controls (brightness, flip, ...) this driver offers - see cam_ctrl.h.
 * *count 0 means none: the page shows no control panel and every request to
 * set one is refused as unknown. */
const cam_ctrl_desc_t *cam_sensor_controls(unsigned *count);

/* Write control @p idx of that table. The value is already range-checked by
 * cam_ctrl. 0 = the write was acknowledged (not read back). Called only on the
 * core that owns the sensor - go through cam_pipeline_set_controls(). */
int  cam_sensor_set_control(unsigned idx, int16_t value);

#endif /* CAM_DRIVER_H */
