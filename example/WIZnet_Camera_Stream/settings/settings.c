/**
 * @file    settings.c
 * @brief   Saved settings - see settings.h.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "settings.h"
#include "settings_flash.h"
#include "settings_store.h"
#include "cam_ctrl.h"
#include "cam_state.h"
#include "cam_pipeline.h"

/*
 * Schema 1: camera part only. Schema 2: camera part + the two accounts.
 * A schema 1 record still loads (no accounts yet); every save writes 2.
 *
 * Text rather than enum values in the camera part, so a record survives a
 * firmware that reorders res_t or a control table: the frame size is its
 * "800x600" string and the controls are the same form body POST /api/cam
 * takes ("brightness=1&flip=0"), parsed back with the same strict parser.
 *
 * The plain HTTP build has no mbedTLS and no login (CAM_AUTH 0). It carries
 * the account block through a save untouched, so switching builds does not
 * lose the accounts.
 */
#define SCHEMA_CAM_ONLY  1u
#define SCHEMA_CAM_AUTH  2u

typedef struct {
    char sensor[16];     /* cam_caps.name it was saved on; "" = none saved */
    char res[12];
    char ctrl[464];
} cam_part_t;

typedef struct {
    cam_part_t cam;
    auth_db_t  auth;
} settings_rec_t;

_Static_assert(sizeof(settings_rec_t) <= SSTORE_PAYLOAD_MAX, "settings record too big");

static sstore_t       s_store;
static bool           s_ready;          /* flash region found                     */
static bool           s_cam_ok;         /* s_rec.cam applies to this sensor       */
static settings_rec_t s_rec;            /* what flash holds (zero when nothing)   */
static const char    *s_note = "not read";
static uint16_t       s_loaded_ver;     /* schema of the record read at boot */
static uint32_t       s_loaded_len;
static sstore_err_t   s_load_err = SSTORE_E_NONE;

static bool str_ok(const char *s, size_t cap)
{
    return memchr(s, 0, cap) != NULL;  /* NUL inside the field */
}

static bool cam_part_ok(const cam_part_t *c)
{
    return str_ok(c->sensor, sizeof(c->sensor)) && str_ok(c->res, sizeof(c->res)) &&
           str_ok(c->ctrl, sizeof(c->ctrl));
}

void settings_boot(void)
{
    memset(&s_rec, 0, sizeof(s_rec));
    s_ready = settings_flash_init();
    if (!s_ready) {
        s_note = "flash not usable";
        return;
    }
    sstore_init(&s_store, settings_flash_ops());

    uint32_t len = 0;
    uint16_t ver = 0;
    settings_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    sstore_err_t e = sstore_load(&s_store, &rec, sizeof(rec), &len, &ver);
    s_load_err = e;
    s_loaded_ver = ver;
    s_loaded_len = len;
    bool shape = (e == SSTORE_OK) &&
                 ((ver == SCHEMA_CAM_ONLY && len == sizeof(cam_part_t)) ||
                  (ver == SCHEMA_CAM_AUTH && len == sizeof(settings_rec_t)));
    if (e != SSTORE_OK) {
        s_note = sstore_err_name(e);
    } else if (!shape || !cam_part_ok(&rec.cam)) {
        s_note = "unknown schema";
    } else {
#if CAM_AUTH
        if (!auth_db_valid(&rec.auth)) {
            /* CRC-checked, so not flash rot: a bug or a foreign record. Keep
             * the camera part, start unprovisioned - only setup mode, which
             * needs physical access, can create the admin again. */
            printf("[settings] account block invalid - ignored\n");
            auth_db_init(&rec.auth);
        }
#endif
        s_rec = rec;
        if (!rec.cam.sensor[0]) {
            s_note = "no camera settings saved";
        } else if (strcmp(rec.cam.sensor, cam_caps.name) != 0) {
            s_note = "saved on another sensor";
        } else {
            s_cam_ok = true;
            s_note = "applied";
        }
    }
    if (s_cam_ok) {
        printf("[settings] record gen %lu (slot %d): res %s, controls \"%s\"\n",
               (unsigned long)s_store.gen, s_store.cur, s_rec.cam.res, s_rec.cam.ctrl);
    } else {
        printf("[settings] camera defaults (%s)\n", s_note);
    }
}

void settings_print(void)
{
    printf("[settings] flash %lu KB, load: %s, slot %d gen %lu, schema %u (%lu B), note: %s,"
           " admin %s\n",
           (unsigned long)(settings_flash_chip_bytes() / 1024), sstore_err_name(s_load_err),
           s_store.cur, (unsigned long)s_store.gen, s_loaded_ver, (unsigned long)s_loaded_len,
           s_note, s_rec.auth.acct[0].present == 1 ? s_rec.auth.acct[0].user : "(none)");
}

bool settings_boot_resolution(res_t *res)
{
    res_t r;
    if (!s_cam_ok || !cam_state_res_parse(s_rec.cam.res, strlen(s_rec.cam.res), &r)) {
        return false;
    }
    if (!cam_sensor_supports(r)) {
        printf("[settings] saved size %s not supported by %s - ignored\n", s_rec.cam.res, cam_caps.name);
        return false;
    }
    *res = r;
    return true;
}

void settings_apply_controls(void)
{
    if (!s_cam_ok || !s_rec.cam.ctrl[0]) {
        return;
    }
    unsigned n = 0;
    const cam_ctrl_desc_t *t = cam_sensor_controls(&n);
    cam_ctrl_batch_t b;
    char bad[CAM_CTRL_NAME_MAX + 1];
    cam_ctrl_err_t e = cam_ctrl_parse_form((const uint8_t *)s_rec.cam.ctrl, strlen(s_rec.cam.ctrl),
                                           t, n, &b, bad, sizeof(bad));
    if (e != CAM_CTRL_OK) {
        /* All or nothing, like a POST: a table that changed under the record
         * does not get half of it. */
        printf("[settings] saved controls not applied: %s (%s)\n", cam_ctrl_err_name(e), bad);
        s_note = "controls not applied";
        return;
    }
    cam_ctrl_result_t res;
    cam_cmd_rc_t rc = cam_pipeline_set_controls(&b, &res, CAM_CMD_TIMEOUT_MS);
    printf("[settings] %u saved control(s): %s\n", (unsigned)b.n, cam_cmd_rc_name(rc));
    if (rc != CAM_CMD_OK) {
        s_note = "controls partly applied";
    }
}

/* The written control values as a form body. */
static bool build_ctrl(char *out, size_t cap)
{
    unsigned n = 0;
    const cam_ctrl_desc_t *t = cam_sensor_controls(&n);
    size_t used = 0;
    out[0] = 0;
    for (unsigned i = 0; t && i < n; i++) {
        int16_t v;
        if (!cam_pipeline_control_written(i, &v)) {
            continue;
        }
        int k = snprintf(out + used, cap - used, "%s%s=%d", used ? "&" : "", t[i].name, v);
        if (k < 0 || (size_t)k >= cap - used) {
            return false;
        }
        used += (size_t)k;
    }
    return true;
}

/* Write @p rec as the new record. Core 1 paused between frames for it. */
static int save_rec(const settings_rec_t *rec, char *why, size_t why_cap, uint32_t *ms)
{
    *ms = 0;
    if (!s_ready) {
        snprintf(why, why_cap, "flash not usable");
        return -1;
    }
    absolute_time_t t0 = get_absolute_time();
    cam_pipeline_pause();               /* between frames, before the lockout */
    sstore_err_t e = sstore_save(&s_store, rec, sizeof(*rec), SCHEMA_CAM_AUTH);
    cam_pipeline_resume();
    *ms = (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
    if (e != SSTORE_OK) {
        snprintf(why, why_cap, "%s", sstore_err_name(e));
        printf("[settings] save failed: %s (%lu ms)\n", why, (unsigned long)*ms);
        return -1;
    }
    s_rec = *rec;
    printf("[settings] saved gen %lu to slot %d in %lu ms\n",
           (unsigned long)s_store.gen, s_store.cur, (unsigned long)*ms);
    return 0;
}

int settings_save_current(char *why, size_t why_cap, uint32_t *ms)
{
    settings_rec_t rec = s_rec;         /* accounts carried over unchanged */
    memset(&rec.cam, 0, sizeof(rec.cam));
    snprintf(rec.cam.sensor, sizeof(rec.cam.sensor), "%s", cam_caps.name);
    snprintf(rec.cam.res, sizeof(rec.cam.res), "%s", cam_state_res_string(cam_state_resolution()));
    if (!build_ctrl(rec.cam.ctrl, sizeof(rec.cam.ctrl))) {
        *ms = 0;
        snprintf(why, why_cap, "control values do not fit the record");
        return -1;
    }
    if (save_rec(&rec, why, why_cap, ms) != 0) {
        return -1;
    }
    s_cam_ok = true;
    s_note = "saved";
    printf("[settings] camera: res %s, controls \"%s\"\n", rec.cam.res, rec.cam.ctrl);
    return 0;
}

const auth_db_t *settings_auth(void)
{
    return &s_rec.auth;
}

int settings_save_auth(const auth_db_t *db, char *why, size_t why_cap, uint32_t *ms)
{
    settings_rec_t rec = s_rec;         /* camera part carried over unchanged */
    rec.auth = *db;
    return save_rec(&rec, why, why_cap, ms);
}

int settings_status_json(char *buf, size_t cap)
{
    int n;
    if (s_cam_ok) {
        n = snprintf(buf, cap,
                     "\"settings\":{\"flash_kb\":%lu,\"gen\":%lu,\"res\":\"%s\",\"ctrl\":\"%s\",\"note\":\"%s\"}",
                     (unsigned long)(settings_flash_chip_bytes() / 1024), (unsigned long)s_store.gen,
                     s_rec.cam.res, s_rec.cam.ctrl, s_note);
    } else {
        n = snprintf(buf, cap, "\"settings\":{\"flash_kb\":%lu,\"gen\":%lu,\"note\":\"%s\"}",
                     (unsigned long)(settings_flash_chip_bytes() / 1024),
                     (unsigned long)s_store.gen, s_note);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
