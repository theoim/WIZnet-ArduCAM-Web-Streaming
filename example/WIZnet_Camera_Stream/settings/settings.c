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

#define SETTINGS_SCHEMA 1u

/*
 * Schema 1, stored as-is. Text rather than enum values, so a record survives a
 * firmware that reorders res_t or a control table: the frame size is its
 * "800x600" string and the controls are the same form body POST /api/cam
 * takes ("brightness=1&flip=0"), parsed back with the same strict parser.
 */
typedef struct {
    char sensor[16];     /* cam_caps.name it was saved on */
    char res[12];
    char ctrl[464];
} settings_v1_t;

_Static_assert(sizeof(settings_v1_t) <= SSTORE_PAYLOAD_MAX, "settings record too big");

static sstore_t      s_store;
static bool          s_ready;          /* flash region found              */
static bool          s_have;           /* s_rec is a record for this sensor */
static settings_v1_t s_rec;
static const char   *s_note = "not read";

static bool str_ok(const char *s, size_t cap)
{
    return memchr(s, 0, cap) != NULL;  /* NUL inside the field */
}

void settings_boot(void)
{
    s_ready = settings_flash_init();
    if (!s_ready) {
        s_note = "flash not usable";
        return;
    }
    sstore_init(&s_store, settings_flash_ops());

    uint32_t len = 0;
    uint16_t ver = 0;
    sstore_err_t e = sstore_load(&s_store, &s_rec, sizeof(s_rec), &len, &ver);
    if (e != SSTORE_OK) {
        s_note = sstore_err_name(e);
    } else if (ver != SETTINGS_SCHEMA || len != sizeof(s_rec) ||
               !str_ok(s_rec.sensor, sizeof(s_rec.sensor)) ||
               !str_ok(s_rec.res, sizeof(s_rec.res)) ||
               !str_ok(s_rec.ctrl, sizeof(s_rec.ctrl))) {
        s_note = "unknown schema";
    } else if (strcmp(s_rec.sensor, cam_caps.name) != 0) {
        s_note = "saved on another sensor";
    } else {
        s_have = true;
        s_note = "applied";
    }
    if (s_have) {
        printf("[settings] record gen %lu (slot %d): res %s, controls \"%s\"\n",
               (unsigned long)s_store.gen, s_store.cur, s_rec.res, s_rec.ctrl);
    } else {
        printf("[settings] no record used (%s) - defaults\n", s_note);
    }
}

bool settings_boot_resolution(res_t *res)
{
    res_t r;
    if (!s_have || !cam_state_res_parse(s_rec.res, strlen(s_rec.res), &r)) {
        return false;
    }
    if (!cam_sensor_supports(r)) {
        printf("[settings] saved size %s not supported by %s - ignored\n", s_rec.res, cam_caps.name);
        return false;
    }
    *res = r;
    return true;
}

void settings_apply_controls(void)
{
    if (!s_have || !s_rec.ctrl[0]) {
        return;
    }
    unsigned n = 0;
    const cam_ctrl_desc_t *t = cam_sensor_controls(&n);
    cam_ctrl_batch_t b;
    char bad[CAM_CTRL_NAME_MAX + 1];
    cam_ctrl_err_t e = cam_ctrl_parse_form((const uint8_t *)s_rec.ctrl, strlen(s_rec.ctrl),
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

int settings_save_current(char *why, size_t why_cap, uint32_t *ms)
{
    settings_v1_t rec;
    *ms = 0;
    if (!s_ready) {
        snprintf(why, why_cap, "flash not usable");
        return -1;
    }
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.sensor, sizeof(rec.sensor), "%s", cam_caps.name);
    snprintf(rec.res, sizeof(rec.res), "%s", cam_state_res_string(cam_state_resolution()));
    if (!build_ctrl(rec.ctrl, sizeof(rec.ctrl))) {
        snprintf(why, why_cap, "control values do not fit the record");
        return -1;
    }

    absolute_time_t t0 = get_absolute_time();
    cam_pipeline_pause();               /* between frames, before the lockout */
    sstore_err_t e = sstore_save(&s_store, &rec, sizeof(rec), SETTINGS_SCHEMA);
    cam_pipeline_resume();
    *ms = (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);

    if (e != SSTORE_OK) {
        snprintf(why, why_cap, "%s", sstore_err_name(e));
        printf("[settings] save failed: %s (%lu ms)\n", why, (unsigned long)*ms);
        return -1;
    }
    s_rec  = rec;
    s_have = true;
    s_note = "saved";
    printf("[settings] saved gen %lu to slot %d in %lu ms: res %s, controls \"%s\"\n",
           (unsigned long)s_store.gen, s_store.cur, (unsigned long)*ms, rec.res, rec.ctrl);
    return 0;
}

int settings_status_json(char *buf, size_t cap)
{
    int n;
    if (s_have) {
        n = snprintf(buf, cap,
                     "\"settings\":{\"flash_kb\":%lu,\"gen\":%lu,\"res\":\"%s\",\"ctrl\":\"%s\",\"note\":\"%s\"}",
                     (unsigned long)(settings_flash_chip_bytes() / 1024), (unsigned long)s_store.gen,
                     s_rec.res, s_rec.ctrl, s_note);
    } else {
        n = snprintf(buf, cap, "\"settings\":{\"flash_kb\":%lu,\"gen\":0,\"note\":\"%s\"}",
                     (unsigned long)(settings_flash_chip_bytes() / 1024), s_note);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
