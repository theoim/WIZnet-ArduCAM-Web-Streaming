/**
 * @file    httpd_stream.h
 * @brief   HTTP + MJPEG server on the lwIP raw TCP API.
 *
 * Serves the same page and the same endpoints as the TOE example. The
 * difference is underneath: here the WIZnet chip runs in MACRAW mode and lwIP
 * terminates TCP in software, instead of the chip terminating it in hardware.
 */

#ifndef HTTPD_STREAM_H
#define HTTPD_STREAM_H

#include <stdbool.h>
#include <stdint.h>
#include "arducam_mega.h"

/** Bind and listen on port 80. Call once, after the netif is up. */
void httpd_stream_init(void);

/**
 * Register the function that pumps the network: drain the chip's receive
 * buffer into lwIP and run its timers.
 *
 * With a hardwired stack the chip keeps acknowledging while the MCU is busy
 * capturing. Here the MCU *is* the stack, so a blocking capture stops TCP dead.
 * Giving the streaming code a way to service the network while it waits for
 * send-buffer space keeps ACKs flowing instead of stalling the connection.
 */
void httpd_stream_set_net_poll(void (*fn)(void));

/**
 * Drive the streaming connection: capture a frame and push it out.
 * Call every main-loop iteration, after sys_check_timeouts().
 */
void httpd_stream_poll(void);

/* Shared with main.c so the banner can report the configured resolution. */
extern volatile bool  g_streaming;
extern volatile res_t g_resolution;

#endif /* HTTPD_STREAM_H */
