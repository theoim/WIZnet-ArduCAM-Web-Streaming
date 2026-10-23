/**
 * @file    cam_sccb.h
 * @brief   SCCB (camera I2C) access shared by every sensor driver.
 *
 * Sensors differ in register address width: the OV2640 and OV7640/7670 use one
 * byte, the OV3660/OV5640, HM01B0 and the Arducam module controller use two.
 * Both forms are here so a driver never has to hand-roll the framing.
 *
 * SCCB is I2C without repeated start: the register index write ends in a STOP
 * and the read is a second transaction. Every read below does exactly that.
 */

#ifndef CAM_SCCB_H
#define CAM_SCCB_H

#include <stdbool.h>
#include <stdint.h>
#include "hardware/i2c.h"

void sccb_init(i2c_inst_t *i2c, int pin_sda, int pin_scl, uint32_t hz);

/** True when a device acknowledges @p addr. Times out instead of hanging. */
bool sccb_present(uint8_t addr);

/* Return 0 on success, negative on a bus error (NACK or timeout). */
int sccb_read8(uint8_t addr, uint8_t reg, uint8_t *val);
int sccb_write8(uint8_t addr, uint8_t reg, uint8_t val);
int sccb_read16(uint8_t addr, uint16_t reg, uint8_t *val);
int sccb_write16(uint8_t addr, uint16_t reg, uint8_t val);

#endif /* CAM_SCCB_H */
