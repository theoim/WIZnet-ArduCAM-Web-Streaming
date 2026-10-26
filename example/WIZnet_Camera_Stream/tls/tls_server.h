/**
 * @file    tls_server.h
 * @brief   TLS on top of the W6300's hardware TCP sockets (camera_stream_https).
 *
 * The chip terminates TCP; mbedTLS runs on the MCU and only ever sees a byte
 * pipe per socket. One context per socket, created when the socket connects and
 * freed when it closes. At most TLS_MAX_CONN at once - each costs about 37 KB of
 * heap (measured in tls_bench), and a fifth connection is closed rather than
 * starving the frame buffer.
 *
 * Everything is non-blocking except tls_send_all(), which spins like the plain
 * tcp_send_all() it replaces. The MJPEG sender uses tls_send_some() instead,
 * so a viewer that reads slowly no longer holds the loop (2026-10-27).
 */

#ifndef TLS_SERVER_H
#define TLS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TLS_MAX_CONN
#define TLS_MAX_CONN   4
#endif

/* false: TLS could not be set up (no RNG, bad cert or key). tls_accept()
 * then refuses every connection instead of serving half a server. */
bool    tls_server_init(void);

bool    tls_active(uint8_t sn);              /* a context exists for this socket */
bool    tls_ready(uint8_t sn);               /* handshake complete               */
bool    tls_accept(uint8_t sn);              /* false: no free slot - close it   */
int     tls_handshake_step(uint8_t sn);      /* 1 done, 0 in progress, <0 failed */

int32_t tls_send_all(uint8_t sn, const uint8_t *buf, uint32_t len);
/* As much as the socket takes now, without waiting: bytes accepted (0 = none
 * yet), <0 failed. After a short count, call again with the rest: mbedTLS may
 * hold a record half written and needs the same remaining bytes back. */
int32_t tls_send_some(uint8_t sn, const uint8_t *buf, uint32_t len);
int32_t tls_recv(uint8_t sn, uint8_t *buf, uint32_t cap);   /* >0, 0 = nothing yet, <0 */
void    tls_close(uint8_t sn);               /* close_notify if possible, free; idempotent */

/* For /api/status. */
int     tls_status_json(char *buf, size_t cap);

/* Random bytes from the TLS DRBG (TRNG-seeded). 0 on success. */
int     tls_random(void *buf, size_t len);

#endif /* TLS_SERVER_H */
