/**
 * @file    ov2640_defs.h
 * @brief   Types and register names shared by the OV2640 driver and its callers.
 *
 * Descended from mega_ccm_regs.h, which described a different chip. The
 * exhibition board carried an ArduCAM Mega CCM: a sensor with a controller in
 * front of it, answering on I2C address 0x1F with sixteen-bit registers that
 * meant things like "resolution number 3". This board carries a bare OV2640,
 * which answers on 0x30 with eight-bit registers split across two banks and no
 * opinion about what a resolution is.
 *
 * The DVP side did not change, which is why the capture path survived the swap
 * intact. Everything in this file is about the control side, which did.
 */

#ifndef __OV2640_DEFS_H__
#define __OV2640_DEFS_H__

#include <stdint.h>

/* ------------------------------------------------------------------- SCCB */

/*
 * Seven bits, because that is what the SDK takes.
 *
 * i2c_write_blocking() and i2c_read_blocking() want the 7-bit address. Datasheets
 * and sample code often print 0x60 for write and 0x61 for read - that is the
 * same address shifted up with the R/W bit attached (0x30 << 1), not a second
 * address. Keeping both spellings in one codebase is how an afternoon
 * disappears, so only the 7-bit form lives here.
 */
#define OV2640_SCCB_ADDR        0x30

/*
 * Bank select, and the one register that exists in both banks.
 *
 * The OV2640 register space is two overlapping pages. 0xFF chooses which one
 * the next access lands in: 0 for the DSP block (JPEG, scaler, output format),
 * 1 for the sensor core (exposure, windowing, clock).
 *
 * Every table below starts by setting it. It is also, awkwardly, the value used
 * to mark the end of a table - see ov2640_regs.h.
 */
#define OV2640_REG_BANK         0xFF
#define OV2640_BANK_DSP         0x00
#define OV2640_BANK_SENSOR      0x01

/* Sensor bank. */
#define OV2640_REG_COM7         0x12    /* bit7 = software reset            */
#define OV2640_REG_CLKRC        0x11    /* internal clock prescaler         */
#define OV2640_REG_COM10        0x15    /* HREF/VSYNC/PCLK polarity         */
#define OV2640_REG_PIDH         0x0A    /* 0x26 on every OV2640             */
#define OV2640_REG_PIDL         0x0B    /* 0x42 on every OV2640             */

#define OV2640_PID_HIGH         0x26
#define OV2640_PID_LOW          0x42

/* DSP bank. */
#define OV2640_REG_IMAGE_MODE   0xDA    /* output format, JPEG enable       */
#define OV2640_REG_R_DVP_SP     0xD3    /* DVP PCLK divider                 */
#define OV2640_REG_RESET        0xE0    /* block-level resets               */

/* ------------------------------------------------------------- frame modes */

typedef enum {
    PIXFORMAT_JPEG   = 1,
    PIXFORMAT_RGB565 = 2,
    PIXFORMAT_YUV422 = 3,
} pixfmt_t;

/*
 * What the OV2640 can actually produce, which is not what the Mega could.
 *
 * Two differences matter to anyone reading the old code: there is no 1280x720,
 * and 1600x1200 is the ceiling rather than a middle step. The exhibition page
 * had HD and FHD buttons; neither size exists here.
 */
typedef enum {
    RES_160X120 = 1,
    RES_176X144,
    RES_320X240,
    RES_352X288,
    RES_640X480,
    RES_800X600,
    RES_1024X768,
    RES_1280X1024,
    RES_1600X1200,
} res_t;

/* --------------------------------------------------------- control writes */

/*
 * The sixteen-bit register form, kept only because cam_controls.c is written
 * against it.
 *
 * None of the constants below exist on an OV2640. They addressed the Mega CCM's
 * controller, and writing them here would put arbitrary values into whatever
 * eight-bit register happens to share the low byte. cam_controls.c therefore
 * refuses to write until Step 7 remaps the table onto the DSP bank; this block
 * exists so the file still compiles while that is outstanding.
 */
struct sensor_reg {
    uint16_t reg;
    uint8_t  val;
};

enum i2c_mode {
    I2C_MODE_8_8  = 0,
    I2C_MODE_16_8 = 1,
};

#define SENSOR_BASE                 0x0100

#define PIXEL_FMT_REG               SENSOR_BASE|0x20
#define RESOLUTION_REG              SENSOR_BASE|0x21
#define BRIGHTNESS_REG              SENSOR_BASE|0x22
#define CONTRAST_REG                SENSOR_BASE|0x23
#define SATURATION_REG              SENSOR_BASE|0x24
#define EXP_COMPENSATE_REG          SENSOR_BASE|0x25
#define AWB_MODE_REG                SENSOR_BASE|0x26
#define SPECIAL_REG                 SENSOR_BASE|0x27
#define SHARPNESS_REG               SENSOR_BASE|0x28
#define FOCUS_REG                   SENSOR_BASE|0x29
#define IMAGE_QUALITY_REG           SENSOR_BASE|0x2A
#define IMAGE_FLIP_REG              SENSOR_BASE|0x2B
#define IMAGE_MIRROR_REG            SENSOR_BASE|0x2C
#define AGC_MODE_REG                SENSOR_BASE|0x30
#define MANUAL_AGC_REG              SENSOR_BASE|0x31
#define MANUAL_EXP_H_REG            SENSOR_BASE|0x33
#define MANUAL_EXP_L_REG            SENSOR_BASE|0x34

#endif /* __OV2640_DEFS_H__ */
