/**
 * @file    webserver.h
 * @brief   The box's control panel: live video, status, and the settings.
 *
 * This server is meant to be published through a port forward on a public
 * address, which makes it a different problem from one on a lab bench.
 *
 * The video runs all the time, and so does the server. An earlier version
 * opened both only during an alarm, on the argument that a port open for an hour
 * a year is a smaller target than one open always. What that cost was the thing
 * the box is for: somebody who wants to know why the basement has been quiet
 * cannot check a panel that only answers during an emergency. A control panel
 * that cannot be checked is not trusted, and one that is not trusted gets
 * replaced by a trip to the basement.
 *
 * What stands in for the closed port is the token. Every path needs it, and a
 * request without it gets 404, not 401 or 403: a scanner should not be able to
 * learn that the path exists, only that it does not. Expect the log to carry
 * those - a forwarded port collects probes for /version, /v2/members and the
 * rest within minutes of being opened, and each of them is this working.
 *
 * What this does not give is confidentiality. It is plain HTTP, so the token
 * travels in the clear and so does the picture. For a basement water tank that
 * is a reasonable trade; for anything where the view itself is sensitive it is
 * not, and the answer there is a tunnel rather than a forwarded port.
 */

#ifndef __WEBSERVER_H__
#define __WEBSERVER_H__

#include <stdbool.h>
#include <stdint.h>

/** Prepare state and open the listening sockets. */
void webserver_init(void);

/**
 * Tell the server whether the tank is in alarm.
 *
 * This drives the BUZZER bar and the note under the picture, and nothing else.
 * It no longer gates the camera: the video is up whether or not anything is
 * wrong, and the alarm is what sends the Discord message and turns the bar red.
 */
void webserver_set_alarm(bool in_alarm);

/** True while the tank is in alarm. The camera runs either way. */
bool webserver_streaming(void);

/**
 * Figures for the status panel: the bin that fired, and the current tone ratio.
 *
 * The ratio is shown while watching rather than only during an alarm, because
 * the number someone needs while deciding where to mount the box is how close
 * the quiet room already sits to the threshold.
 */
void webserver_set_detail(int hz, float tone);

/** Service all sockets once. Non-blocking; call from the main loop. */
void webserver_poll(void);

/** Seconds since the last request from anybody, for the linger timer. */
uint32_t webserver_idle_seconds(void);

#endif /* __WEBSERVER_H__ */
