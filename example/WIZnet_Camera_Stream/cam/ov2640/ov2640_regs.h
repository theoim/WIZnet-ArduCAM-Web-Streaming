/**
 * @file    ov2640_regs.h
 * @brief   OV2640 register tables, taken from a working build.
 *
 * Source: theoim/ArduCAM_RP2040_C, W55RP20_ArduCAM_Example/ArduCAM/ov2640_regs.c
 *
 * That project drives an ArduCAM Mini - SPI, with an ArduChip and a FIFO in
 * front of the sensor. None of that transport applies here and none of it was
 * copied. What was copied is the part that is about the OV2640 itself: the
 * sequence of SCCB writes that turns it into a JPEG source at a given size.
 * Those are identical whether the bytes leave over SPI or over DVP.
 *
 * The tables are verbatim. Changing a value here means disagreeing with a
 * configuration that is known to produce pictures, which is worth doing only
 * with a measurement in hand.
 */

#ifndef __OV2640_REGS_H__
#define __OV2640_REGS_H__

#include <stdint.h>

/*
 * Eight bits each, unlike the sixteen-bit struct sensor_reg in ov2640_defs.h.
 * The two are not interchangeable and the difference is deliberate: this one
 * describes the OV2640, that one describes the controller this board does not
 * have.
 */
typedef struct {
    uint8_t reg;
    uint8_t val;
} ov2640_reg_t;

/*
 * A table ends at {0xff, 0xff}, and 0xff is also the bank-select register.
 *
 * So the terminator cannot be recognised by the register alone - every table
 * legitimately writes 0xff several times to move between the DSP and sensor
 * banks. Both bytes have to match:
 *
 *     while (t->reg != 0xff || t->val != 0xff) { ... }
 *
 * Getting this wrong writes 0xff into the bank select and leaves the sensor
 * pointing at a page that does not exist.
 */
#define OV2640_TABLE_END_REG    0xFF
#define OV2640_TABLE_END_VAL    0xFF

extern const ov2640_reg_t OV2640_QVGA[];
extern const ov2640_reg_t OV2640_JPEG_INIT[];
extern const ov2640_reg_t OV2640_YUV422[];
extern const ov2640_reg_t OV2640_JPEG[];
extern const ov2640_reg_t OV2640_160x120_JPEG[];
extern const ov2640_reg_t OV2640_176x144_JPEG[];
extern const ov2640_reg_t OV2640_320x240_JPEG[];
extern const ov2640_reg_t OV2640_352x288_JPEG[];
extern const ov2640_reg_t OV2640_640x480_JPEG[];
extern const ov2640_reg_t OV2640_800x600_JPEG[];
extern const ov2640_reg_t OV2640_1024x768_JPEG[];
extern const ov2640_reg_t OV2640_1280x1024_JPEG[];
extern const ov2640_reg_t OV2640_1600x1200_JPEG[];

#endif /* __OV2640_REGS_H__ */
