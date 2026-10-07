/**
 * @file    cam_controls.h
 * @brief   One table of sensor controls; the query parser, the status JSON and
 *          the browser panel are all generated from it.
 *
 * The page does not know what controls exist. It fetches /api/controls at load,
 * gets name / label / group / range for each row, and builds the panel from
 * that. Adding a row in cam_controls.c makes a new slider appear in the browser
 * with no edit to the HTML and no edit to the router.
 *
 * Written three separate times instead - a parser, a JSON writer and a block of
 * HTML - the three lists would drift, and the one that drifted would be the
 * HTML, because it is the one nothing compiles.
 */

#ifndef __CAM_CONTROLS_H__
#define __CAM_CONTROLS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;       /* query key, e.g. "brightness"    */
    const char *label;      /* what the page shows             */
    const char *group;      /* panel column                    */
    uint16_t    reg;        /* ArduCAM MEGA CCM register       */
    int16_t     min;
    int16_t     max;        /* min==0 && max==1 renders as a checkbox */
    int16_t     def;
} cam_ctrl_t;

/** Number of rows in the table. */
int                cam_controls_count(void);

/** Row @p i, or NULL when out of range. */
const cam_ctrl_t  *cam_controls_at(int i);

/** Current value of row @p i. */
int16_t            cam_controls_value(int i);

/**
 * Push every control's current value to the sensor.
 *
 * Call after anything that re-initialises it - a resolution change or a
 * recover - or the sensor silently reverts to its own defaults and the panel
 * then shows values the hardware does not have.
 */
void               cam_controls_apply_all(void);

/**
 * Apply every key found in a query string slice, e.g. "brightness=4&contrast=2".
 * The slice is not NUL-terminated; it points into the request line.
 *
 * @param[out] refused  set when a value was out of range and was clamped or
 *                      rejected. The caller answers 409 so the page can say the
 *                      sensor refused it instead of silently snapping the
 *                      slider back.
 * @return number of controls actually written.
 */
int                cam_controls_apply_query(const char *q, size_t qlen,
                                            bool *refused);

/**
 * Write the control descriptors as a JSON array - what /api/controls answers.
 * @return bytes written, or a negative value if @p cap was too small.
 */
int                cam_controls_json_descriptors(char *buf, size_t cap);

/**
 * Write the current values as a JSON object body, e.g.
 * `"brightness":4,"contrast":2` - no braces, so the caller can embed it.
 * @return bytes written, or a negative value if @p cap was too small.
 */
int                cam_controls_json_values(char *buf, size_t cap);

#endif /* __CAM_CONTROLS_H__ */
