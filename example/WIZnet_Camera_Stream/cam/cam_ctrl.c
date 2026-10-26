/**
 * @file    cam_ctrl.c
 * @brief   Control table lookup, strict value parsing, batches, written values.
 *
 * See cam_ctrl.h for the rules. Depends on string.h only.
 */

#include <string.h>

#include "cam_ctrl.h"

const char *cam_ctrl_err_name(cam_ctrl_err_t e)
{
    switch (e) {
    case CAM_CTRL_OK:         return "ok";
    case CAM_CTRL_E_UNKNOWN:  return "unknown control";
    case CAM_CTRL_E_SYNTAX:   return "not a valid value";
    case CAM_CTRL_E_RANGE:    return "out of range";
    case CAM_CTRL_E_DUP:      return "control given twice";
    case CAM_CTRL_E_TOO_MANY: return "too many controls in one request";
    }
    return "?";
}

int cam_ctrl_find(const cam_ctrl_desc_t *t, unsigned n, const char *name, size_t len)
{
    if (!t || !name || len == 0 || len > CAM_CTRL_NAME_MAX) {
        return -1;
    }
    for (unsigned i = 0; i < n && i < CAM_CTRL_MAX; i++) {
        if (strlen(t[i].name) == len && memcmp(t[i].name, name, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Decimal with an optional '-'. Five digits at most: every range here fits in
 * int16_t, and a longer string is a typo or a probe, not a value.
 */
static bool parse_decimal(const char *s, size_t len, int32_t *out)
{
    size_t i = 0;
    bool   neg = false;

    if (len == 0) {
        return false;
    }
    if (s[0] == '-') {
        neg = true;
        i = 1;
    }
    size_t digits = len - i;
    if (digits == 0 || digits > 5) {
        return false;
    }
    if (s[i] == '0' && (digits > 1 || neg)) {
        return false;                       /* "007", "-0" */
    }
    int32_t v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (int32_t)(s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

cam_ctrl_err_t cam_ctrl_parse_value(const cam_ctrl_desc_t *d, const char *s, size_t len,
                                    int16_t *out)
{
    int32_t v;

    if (!d || !s) {
        return CAM_CTRL_E_SYNTAX;
    }
    if (!parse_decimal(s, len, &v)) {
        if (d->type != CAM_CTRL_ENUM || !d->enum_names) {
            return CAM_CTRL_E_SYNTAX;
        }
        for (int32_t k = 0; k <= d->max; k++) {
            const char *nm = d->enum_names[k];
            if (nm && strlen(nm) == len && memcmp(nm, s, len) == 0) {
                *out = (int16_t)k;
                return CAM_CTRL_OK;
            }
        }
        return CAM_CTRL_E_SYNTAX;
    }
    if (v < d->min || v > d->max) {
        return CAM_CTRL_E_RANGE;
    }
    *out = (int16_t)v;
    return CAM_CTRL_OK;
}

void cam_ctrl_batch_init(cam_ctrl_batch_t *b)
{
    memset(b, 0, sizeof(*b));
}

cam_ctrl_err_t cam_ctrl_batch_add(cam_ctrl_batch_t *b, const cam_ctrl_desc_t *t, unsigned n,
                                  const char *name, size_t name_len,
                                  const char *val, size_t val_len)
{
    int idx = cam_ctrl_find(t, n, name, name_len);
    if (idx < 0) {
        return CAM_CTRL_E_UNKNOWN;
    }
    for (unsigned i = 0; i < b->n; i++) {
        if (b->item[i].idx == (uint8_t)idx) {
            return CAM_CTRL_E_DUP;
        }
    }
    if (b->n >= CAM_CTRL_BATCH_MAX) {
        return CAM_CTRL_E_TOO_MANY;
    }
    int16_t v;
    cam_ctrl_err_t e = cam_ctrl_parse_value(&t[idx], val, val_len, &v);
    if (e != CAM_CTRL_OK) {
        return e;
    }
    b->item[b->n].idx   = (uint8_t)idx;
    b->item[b->n].value = v;
    b->n++;
    return CAM_CTRL_OK;
}

void cam_ctrl_run(const cam_ctrl_batch_t *b, cam_ctrl_setter_t set, cam_ctrl_result_t *res)
{
    res->n_ok      = 0;
    res->failed_at = -1;
    res->err       = 0;
    for (uint8_t i = 0; i < b->n; i++) {
        int rc = set(b->item[i].idx, b->item[i].value);
        if (rc != 0) {
            res->failed_at = (int8_t)i;
            res->err       = rc;
            return;
        }
        res->n_ok++;
    }
}

void cam_ctrl_written_clear(cam_ctrl_written_t *w)
{
    memset(w, 0, sizeof(*w));
}

void cam_ctrl_written_record(cam_ctrl_written_t *w, const cam_ctrl_batch_t *b,
                             const cam_ctrl_result_t *res)
{
    for (uint8_t i = 0; i < res->n_ok && i < b->n; i++) {
        uint8_t idx = b->item[i].idx;
        if (idx >= CAM_CTRL_MAX) {
            continue;
        }
        w->value[idx] = b->item[i].value;
        w->mask |= 1u << idx;
    }
}

bool cam_ctrl_written_get(const cam_ctrl_written_t *w, unsigned idx, int16_t *out)
{
    if (idx >= CAM_CTRL_MAX || !(w->mask & (1u << idx))) {
        return false;
    }
    *out = w->value[idx];
    return true;
}
