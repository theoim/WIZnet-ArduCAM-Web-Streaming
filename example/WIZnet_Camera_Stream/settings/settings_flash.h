/**
 * @file    settings_flash.h
 * @brief   The settings store's two sectors in the RP2350's QSPI flash.
 *
 * The region is two 4 KB sectors just below the last sector of the chip
 * actually fitted, found from its JEDEC ID at boot - not PICO_FLASH_SIZE_BYTES.
 * The last sector itself is erased by every UF2 download (see settings_flash.c). This build uses
 * PICO_BOARD=pico2, which says 4 MB, while the W6300-EVB-Pico2 carries a 2 MB
 * (16 Mbit) part: an offset computed from the SDK value would land past the
 * end of the chip and wrap.
 */

#ifndef SETTINGS_FLASH_H
#define SETTINGS_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "settings_store.h"

/**
 * Read the JEDEC ID and place the region. Call once at boot, before core 1 is
 * launched (the ID read takes the flash out of XIP). false - and every later
 * operation fails - when the ID is not a plausible size or the firmware image
 * reaches into the region.
 */
bool settings_flash_init(void);

/** The callbacks for sstore_init(). Erase/program go through
 *  flash_safe_execute(), so core 1 must be a lockout victim. */
const sstore_flash_t *settings_flash_ops(void);

uint32_t settings_flash_chip_bytes(void);    /* 0 before a successful init */
uint32_t settings_flash_region_off(void);    /* offset of slot 0 in flash  */

#endif /* SETTINGS_FLASH_H */
