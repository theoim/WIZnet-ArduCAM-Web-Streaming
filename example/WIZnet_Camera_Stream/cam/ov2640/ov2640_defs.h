/**
 * @file    ov2640_defs.h
 * @brief   OV2640 SCCB address and the registers this driver touches.
 *
 * The OV2640 has two register banks selected through 0xFF. Everything the
 * exhibition build learned about them is in ov2640.c; this is only the names.
 */

#ifndef OV2640_DEFS_H
#define OV2640_DEFS_H

#define OV2640_SCCB_ADDR        0x30

#define OV2640_REG_BANK         0xFF
#define OV2640_BANK_DSP         0x00
#define OV2640_BANK_SENSOR      0x01

/* Sensor bank. */
#define OV2640_REG_COM7         0x12    /* bit7 = software reset            */
#define OV2640_REG_CLKRC        0x11    /* internal clock prescaler         */
#define OV2640_REG_COM10        0x15    /* HREF/VSYNC/PCLK polarity         */
#define OV2640_REG_PIDH         0x0A
#define OV2640_REG_PIDL         0x0B

#define OV2640_PID_HIGH         0x26
#define OV2640_PID_LOW          0x42

/* DSP bank. */
#define OV2640_REG_R_DVP_SP     0xD3    /* DVP PCLK divider                 */

#endif /* OV2640_DEFS_H */
