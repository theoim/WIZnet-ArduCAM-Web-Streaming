/**
 * @file    settings.h
 * @brief   The camera's saved settings: what is kept, and when it is read.
 *
 * The record keeps the frame size and the control values that had been
 * written when the admin pressed Save (schema 1), and the two accounts
 * (schema 2, Phase 3b). Nothing is saved automatically: a change made
 * on the page lasts until reboot unless it is saved (decision Q7, 2026-10-23).
 *
 * A record is applied only on the sensor it was saved on - an OV2640 record
 * on an LH board is ignored, not half-applied.
 *
 *   boot     settings_boot()            read the record (no writes)
 *            settings_boot_resolution() frame size to start with, if saved
 *            settings_apply_controls()  after the size is set, before core 1
 *   runtime  settings_save_current()    core 0, from the HTTP handler
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cam_driver.h"
#include "auth.h"

#ifndef CAM_AUTH
#define CAM_AUTH 0      /* 1 in the HTTPS build: login, accounts checked */
#endif

void settings_boot(void);

/** One line on what was read at boot - again, for when the boot log was missed. */
void settings_print(void);

/** true and *res when the saved record names a size this sensor supports. */
bool settings_boot_resolution(res_t *res);

/** Write the saved control values through the normal control path. */
void settings_apply_controls(void);

/**
 * Save the current frame size and written control values. Core 0 only; core 1
 * is paused between frames for the flash work. Returns 0, or -1 with a reason
 * in @p why. *ms is how long the save held core 0.
 */
int settings_save_current(char *why, size_t why_cap, uint32_t *ms);

/** The saved accounts (all empty when nothing is saved). */
const auth_db_t *settings_auth(void);

/** Save new accounts; the camera part of the record is kept as it is. */
int settings_save_auth(const auth_db_t *db, char *why, size_t why_cap, uint32_t *ms);

/** "settings":{...} member for the status document (no leading comma). */
int settings_status_json(char *buf, size_t cap);

#endif /* SETTINGS_H */
