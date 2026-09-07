/**
 * @file    cam_state.h
 * @brief   Everything the two servers share: capture state, the one-second
 *          metrics window, and the status document.
 *
 * Both stacks emit byte-identical JSON because both call the same builder here.
 * That is not tidiness - the whole exhibit is two boards being read against
 * each other, and a field that differed in name or rounding between them would
 * put the difference in the page instead of in the network stack.
 */

#ifndef __CAM_STATE_H__
#define __CAM_STATE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mega_ccm_regs.h"      /* res_t */

/* --------------------------------- Capture --------------------------------- */
void        cam_state_init(void);

bool        cam_state_streaming(void);
void        cam_state_set_streaming(bool on);

res_t       cam_state_resolution(void);
const char *cam_state_res_string(res_t res);
bool        cam_state_res_parse(const char *s, size_t len, res_t *out);
void        cam_state_apply_resolution(res_t res);

/** Reinitialise a wedged sensor and push the control table back into it. */
void        cam_state_recover(void);

/* --------------------------------- Counters -------------------------------- */
void        cam_state_note_dropped(void);

/**
 * A capture failed. Returns true when the failure streak reached the recovery
 * threshold and the sensor was reset, so the caller can log it.
 */
bool        cam_state_note_capture_fail(void);
void        cam_state_note_capture_ok(void);

/**
 * Record one frame that reached the peer.
 * @param send_us  time spent in the network, measured by the caller - it is the
 *                 one part of the frame period the two stacks do differently.
 */
void        cam_state_note_frame(uint64_t send_us, uint32_t bytes);

/**
 * Record time spent waiting for the peer to acknowledge a frame.
 *
 * This is the part of the frame period that was invisible. The three reported
 * timings summed to about 52 ms while the measured rate was 9.6 fps - a 104 ms
 * period - so the page was naming a bottleneck out of half the period it could
 * see, and naming it wrongly.
 *
 * On lwIP the next capture cannot start until the last frame is acknowledged,
 * because lwIP holds a pointer into image_buff rather than a copy. On the TOE
 * the chip owns the buffer and there is no such wait, so this stays zero -
 * which makes it the one number that shows the cost of a software stack
 * directly rather than by subtraction.
 */
void        cam_state_note_drain(uint64_t drain_us);

/** Discard the window in progress. Call when the workload changes. */
void        cam_state_metrics_reset(void);

/** Roll the window over into the reported values once a second has passed. */
void        cam_state_metrics_update(void);

/* -------------------------------- Reporting -------------------------------- */
/*
 * Big enough for the fixed fields plus the whole control table. The status
 * document carries every control's current value so that one poll re-renders
 * the entire panel - the page never has to work out what changed.
 */
#define CAM_STATUS_JSON_MAX  768

/**
 * Build the status document.
 * @return bytes written, or a negative value if @p cap was too small.
 */
int         cam_state_status_json(char *buf, size_t cap);

#endif /* __CAM_STATE_H__ */
