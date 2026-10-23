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
 * tcp_send_all() it replaces: a frame going out holds the loop either way.
 */

#ifndef TLS_SERVER_H
#define TLS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TLS_MAX_CONN
#define TLS_MAX_CONN   4
#endif

void    tls_server_init(void);

bool    tls_active(uint8_t sn);              /* a context exists for this socket */
bool    tls_ready(uint8_t sn);               /* handshake complete               */
bool    tls_accept(uint8_t sn);              /* false: no free slot - close it   */
int     tls_handshake_step(uint8_t sn);      /* 1 done, 0 in progress, <0 failed */

int32_t tls_send_all(uint8_t sn, const uint8_t *buf, uint32_t len);
int32_t tls_recv(uint8_t sn, uint8_t *buf, uint32_t cap);   /* >0, 0 = nothing yet, <0 */
void    tls_close(uint8_t sn);               /* close_notify if possible, free; idempotent */

/* For /api/status. */
int     tls_status_json(char *buf, size_t cap);

#endif /* TLS_SERVER_H */
