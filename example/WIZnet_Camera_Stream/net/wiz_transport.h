/**
 * @file    wiz_transport.h
 * @brief   Connections on the WIZnet hardware TCP/IP stack: chip bring-up,
 *          listening sockets, TLS or plain, deadlines, one way to close.
 *
 * Step 1 (2026-11-05): implemented in wiz_transport.c; server_toe.c is its
 * only user until the HTTP layer moves out (step 2).
 *
 * What it owns
 *   - the chip and its addressing
 *   - every listening socket on one port, and socket recycling
 *     (CLOSE_WAIT, LAST_ACK, TIME_WAIT, forced close)
 *   - TLS per connection when built with it, so the layer above reads and
 *     writes plaintext either way
 *   - connection lifetime and deadlines, and the pool of request slots that
 *     goes with them (conn.c, unit-tested; kept as it is)
 *
 * What it does not know: HTTP, cameras, accounts. Socket numbers, conn.c
 * and ioLibrary types stay inside; the layer above sees a connection index,
 * an opaque request slot number and the types below.
 *
 * Start-up order:
 *     wt_handlers_t h;
 *     http_server_init(&http_cfg, &h);     // fills h
 *     wt_init(&wt_cfg, &h);                // chip, sockets, TLS
 *     for (;;) wt_service();
 *
 * Memory: all static, sized at build time. No allocation.
 */

#ifndef WIZ_TRANSPORT_H
#define WIZ_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WT_MAX_CONN              8  /* hardware sockets used, one port      */
#define WT_MAX_REQUEST_SLOTS     4  /* connections receiving a request at
                                       once; = TLS sessions on HTTPS        */

typedef uint8_t wt_conn_t;          /* 0..WT_MAX_CONN-1                     */
typedef uint8_t wt_slot_t;          /* 0..WT_MAX_REQUEST_SLOTS-1            */

/* Why a connection ended. Counted per reason for /api/status. */
typedef enum {
    WT_CLOSE_DONE = 0,          /* response sent, Connection: close          */
    WT_CLOSE_PARSE,             /* request rejected by the parser            */
    WT_CLOSE_TIMEOUT_HS,        /* TLS handshake ran out                     */
    WT_CLOSE_TIMEOUT_REQ,       /* request total or no-progress ran out      */
    WT_CLOSE_TLS,               /* TLS error / handshake failure             */
    WT_CLOSE_TLS_REFUSED,       /* no TLS session free, or TLS not available */
    WT_CLOSE_PEER,              /* peer FIN / RST                            */
    WT_CLOSE_TAKEOVER,          /* replaced by a newer connection            */
    WT_CLOSE_SEND,              /* send failed or stalled                    */
    WT_CLOSE_POOL,              /* no request slot free                      */
    WT_CLOSE_REASONS_
} wt_close_reason_t;

typedef struct {
    uint8_t  mac[6];
    uint8_t  ip[4], mask[4], gw[4], dns[4];
    uint16_t port;              /* 443 with TLS, 80 without                  */
    uint8_t  first_socket;      /* hardware socket of connection 0           */
    bool     tls;               /* must match the build (STREAM_HTTPS)       */
} wt_config_t;

/*
 * Called from wt_service(), on core 0, one connection at a time; while one
 * runs, no other socket is polled.
 *
 * Blocking: a callback may send a response with wt_send_all(), which waits
 * for TX space for up to 2 s without progress. That is the server's behaviour
 * today and stays so for this split; it is the place to change when responses
 * become non-blocking. Nothing else in a callback may wait.
 *
 * `slot` is valid from on_ready until the connection is held or closed.
 */
typedef struct {
    /* Ready for application bytes (after the TLS handshake, if any). */
    void (*on_ready)(void *user, wt_conn_t c, wt_slot_t slot);

    /* Possibly bytes to read; read them with wt_recv(). Only while the
     * connection is waiting for or receiving a request. */
    void (*on_readable)(void *user, wt_conn_t c, wt_slot_t slot);

    /* A deadline ran out while a request was arriving: last chance to send a
     * status (408). The connection is closed right after. May be NULL. */
    void (*on_request_timeout)(void *user, wt_conn_t c, wt_slot_t slot);

    /* No request slot free for a new connection: last chance to send a
     * status (503). The connection is closed right after. May be NULL. */
    void (*on_refused)(void *user, wt_conn_t c);

    /* A held connection (see wt_hold) gets this on every pass. */
    void (*on_hold)(void *user, wt_conn_t c);

    /* The connection is gone, for any reason - exactly once per connection.
     * Release whatever was tied to it. */
    void (*on_closed)(void *user, wt_conn_t c, wt_close_reason_t why);

    void *user;
} wt_handlers_t;

/* Chip, addressing, TLS context, listeners. Blocks until the chip answers.
 * The handlers are copied. */
bool    wt_init(const wt_config_t *cfg, const wt_handlers_t *h);

/* One pass over every connection: deadlines, socket state, TLS steps, then
 * the callbacks above. Call in the main loop. */
void    wt_service(void);

/* Send everything, waiting for TX space; gives up after 2 s without
 * progress (bounded blocking, see above). Bytes sent, or <0. */
int32_t wt_send_all(wt_conn_t c, const void *buf, uint32_t len);

/* Queue what fits now and return. 0 = no space yet; <0 = connection gone.
 * After a short count, call again with the same remaining bytes (TLS may
 * hold part of a record). For the MJPEG stream. */
int32_t wt_send_some(wt_conn_t c, const void *buf, uint32_t len);

/* Plaintext bytes received, 0 when nothing is waiting, <0 when closed. */
int32_t wt_recv(wt_conn_t c, void *buf, uint32_t cap);

/* Turn a request connection into a long-lived one: request deadlines stop,
 * its request slot goes back to the pool (the slot number is no longer
 * valid), on_hold runs from now on. */
void    wt_hold(wt_conn_t c);

/* Close. Idempotent: the first call does the work (TLS close_notify, FIN,
 * CLOSING deadline), later calls do nothing. on_closed follows. */
void    wt_close(wt_conn_t c, wt_close_reason_t why);

bool    wt_is_held(wt_conn_t c);
const char *wt_close_reason_name(wt_close_reason_t why);

/* "\"http\":{...},\"slow\":{...}," fragment: close reasons, forced closes,
 * slowest pass, listener gaps. */
int     wt_status_json(char *buf, size_t cap);

#endif /* WIZ_TRANSPORT_H */
