/**
 * @file    cam_ctrl.h
 * @brief   Sensor controls: what a driver offers, how a request is checked, and
 *          what is known about the values afterwards.
 *
 * Three rules, from the Phase 2 brief:
 *
 *   1. Nothing is clamped. A value outside a control's range is refused with
 *      an error naming the control; it is never quietly moved to the nearest
 *      legal value. The caller must be told that what it asked for did not
 *      happen.
 *   2. A batch is checked completely before anything is written. A request
 *      with one bad value writes none of its values.
 *   3. "Written" means the register write was acknowledged, not that the
 *      sensor reads the value back or that the picture changed. Nothing here
 *      is presented as a readback. After a sensor reset or a frame-size change
 *      the written values are forgotten, because whether the sensor kept them
 *      is not known.
 *
 * No allocation. A driver describes its controls in a const table; a request
 * fills a fixed-size batch; the record of written values is one small struct.
 * This file has no Pico SDK dependency and is built on the host by
 * test/run_host_tests.sh.
 */

#ifndef CAM_CTRL_H
#define CAM_CTRL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAM_CTRL_MAX        16      /* controls one driver can describe      */
#define CAM_CTRL_BATCH_MAX  8       /* controls one request can set          */
#define CAM_CTRL_NAME_MAX   24      /* longest control name, without the NUL */

typedef enum {
    CAM_CTRL_INT = 0,               /* integer in [min, max]                 */
    CAM_CTRL_BOOL,                  /* 0 or 1                                */
    CAM_CTRL_ENUM,                  /* index into enum_names, 0..max         */
} cam_ctrl_type_t;

/* The range came from an example or a vendor header, not from a datasheet or a
 * measurement. The page shows it as such. */
#define CAM_CTRL_F_PROVISIONAL  0x01u

typedef struct {
    const char         *name;       /* form field and JSON key: [a-z0-9_]+   */
    const char         *label;      /* for the page                          */
    uint8_t             type;       /* cam_ctrl_type_t                       */
    uint8_t             flags;      /* CAM_CTRL_F_*                          */
    int16_t             min;        /* inclusive; BOOL 0, ENUM 0             */
    int16_t             max;        /* inclusive; BOOL 1, ENUM count - 1     */
    const char *const  *enum_names; /* ENUM only: max + 1 labels             */
    uint32_t            priv;       /* the driver's own (register address)   */
} cam_ctrl_desc_t;

typedef enum {
    CAM_CTRL_OK = 0,
    CAM_CTRL_E_UNKNOWN,             /* no control by that name               */
    CAM_CTRL_E_SYNTAX,              /* value is not a number / known label   */
    CAM_CTRL_E_RANGE,               /* a number, but outside [min, max]      */
    CAM_CTRL_E_DUP,                 /* same control twice in one request     */
    CAM_CTRL_E_TOO_MANY,            /* more than CAM_CTRL_BATCH_MAX          */
    CAM_CTRL_E_EMPTY,               /* a request that names no control       */
} cam_ctrl_err_t;

const char *cam_ctrl_err_name(cam_ctrl_err_t e);

/** Index of the control called @p name, or -1. Exact, case-sensitive. */
int cam_ctrl_find(const cam_ctrl_desc_t *t, unsigned n, const char *name, size_t len);

/**
 * Turn the text of a value into a number for control @p d.
 *
 * INT/BOOL take a decimal: an optional '-', then digits, no '+', no spaces,
 * no leading zeros. ENUM takes either its index in that form or one of its
 * labels exactly. The result is range-checked; out of range is
 * CAM_CTRL_E_RANGE and *out is left alone.
 */
cam_ctrl_err_t cam_ctrl_parse_value(const cam_ctrl_desc_t *d, const char *s, size_t len,
                                    int16_t *out);

/* ------------------------------- Batches ---------------------------------- */
typedef struct {
    uint8_t idx;                    /* into the driver's table               */
    int16_t value;                  /* already range-checked                 */
} cam_ctrl_item_t;

typedef struct {
    uint8_t         n;
    cam_ctrl_item_t item[CAM_CTRL_BATCH_MAX];
} cam_ctrl_batch_t;

void cam_ctrl_batch_init(cam_ctrl_batch_t *b);

/**
 * Check one name=value pair and add it to @p b. On any error @p b is
 * unchanged, so a caller can stop at the first bad pair and write nothing.
 */
cam_ctrl_err_t cam_ctrl_batch_add(cam_ctrl_batch_t *b, const cam_ctrl_desc_t *t, unsigned n,
                                  const char *name, size_t name_len,
                                  const char *val, size_t val_len);

/**
 * Parse an application/x-www-form-urlencoded body ("brightness=3&flip=1")
 * into @p b. Names and values are percent-decoded ('+' is a space). Strict:
 * an empty pair, a pair without '=', or a bad escape is CAM_CTRL_E_SYNTAX; no
 * pairs at all is CAM_CTRL_E_EMPTY. Stops at the first error and copies the
 * offending name (decoded, truncated) into @p bad_name so the caller can say
 * which control was refused. @p b is only meaningful on CAM_CTRL_OK.
 */
cam_ctrl_err_t cam_ctrl_parse_form(const uint8_t *body, size_t len,
                                   const cam_ctrl_desc_t *t, unsigned n,
                                   cam_ctrl_batch_t *b, char *bad_name, size_t bad_cap);

/* ------------------------------- Applying --------------------------------- */
/**
 * Write one control. Returns 0 when the write was acknowledged. Called on the
 * core that owns the sensor at that moment.
 */
typedef int (*cam_ctrl_setter_t)(unsigned idx, int16_t value);

typedef struct {
    uint8_t n_ok;                   /* items [0, n_ok) were written          */
    int8_t  failed_at;              /* first item that failed, or -1         */
    int     err;                    /* the setter's return for failed_at     */
} cam_ctrl_result_t;

/**
 * Write the batch in order and stop at the first failure. Items after it are
 * not attempted - the sensor is in an unknown state at that point and more
 * writes would not make it less unknown.
 */
void cam_ctrl_run(const cam_ctrl_batch_t *b, cam_ctrl_setter_t set, cam_ctrl_result_t *res);

/* ---------------------------- Written values ------------------------------ */
typedef struct {
    uint32_t mask;                  /* bit i: control i has a written value  */
    int16_t  value[CAM_CTRL_MAX];
} cam_ctrl_written_t;

void cam_ctrl_written_clear(cam_ctrl_written_t *w);

/** Record items [0, res->n_ok) of @p b. */
void cam_ctrl_written_record(cam_ctrl_written_t *w, const cam_ctrl_batch_t *b,
                             const cam_ctrl_result_t *res);

/** The last written value of control @p idx; false when there is none. */
bool cam_ctrl_written_get(const cam_ctrl_written_t *w, unsigned idx, int16_t *out);

#endif /* CAM_CTRL_H */
