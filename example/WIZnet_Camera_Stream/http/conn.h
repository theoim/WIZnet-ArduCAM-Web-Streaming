/**
 * @file    conn.h
 * @brief   Connection lifetime for the camera server: phases, deadlines,
 *          request-context pool, idempotent close. No SDK, no sockets.
 *
 * Phase 1b. Until 2026-10 a connection had no lifetime of its own: a client
 * that connected and sent nothing, or stopped half way through a handshake,
 * held a hardware socket and a ~37 KB TLS session until it went away by
 * itself. With four TLS sessions that is four idle connections from a full
 * denial of the camera. This module decides when a connection has had long
 * enough; server_toe.c does the socket and TLS work it asks for.
 *
 * Phases and the clock that applies to each:
 *
 *   FREE       no connection (listening)                       -
 *   TLS        handshake in progress (HTTPS only)              total CONN_HS_TOTAL_MS
 *   WAIT_REQ   ready for a request, none of it arrived yet     total CONN_REQ_TOTAL_MS
 *   REQ        request bytes arriving                          same total (not reset)
 *                                                              + no-progress CONN_REQ_IDLE_MS
 *   STREAM     MJPEG stream                                    none here (the 2 s send
 *                                                              stall in the sender applies)
 *   CLOSING    disconnect issued, waiting for CLOSED           total CONN_CLOSING_MS, then
 *                                                              the caller forces close()
 *
 * The request total starts when the connection becomes ready for a request
 * and is never extended: a client that trickles one byte a second still runs
 * out at CONN_REQ_TOTAL_MS. The no-progress limit catches a client that sent
 * part of a request and stopped.
 *
 * There is no separate "connected, TLS not started" phase: the server starts
 * TLS (or refuses the connection) on the same pass that sees it ESTABLISHED.
 *
 * Time is a free-running 32-bit millisecond counter supplied by the caller.
 * Every comparison is (uint32_t)(now - then) against a limit, which is correct
 * across the 49.7-day wrap for any interval under 2^31 ms.
 *
 * Request contexts: CONN_REQ_POOL of them, owned by at most one connection
 * each, handed out when a connection becomes ready for a request and taken
 * back when it streams or closes. The pool size matches the TLS session limit
 * on the HTTPS image, so a TLS connection always gets one.
 */

#ifndef CONN_H
#define CONN_H

#include <stdbool.h>
#include <stdint.h>

#ifndef CONN_MAX_SOCKETS
#define CONN_MAX_SOCKETS    8       /* W6300 hardware sockets polled       */
#endif
#ifndef CONN_REQ_POOL
#define CONN_REQ_POOL       4       /* = TLS_MAX_CONN on the HTTPS image   */
#endif
#ifndef CONN_HS_TOTAL_MS
#define CONN_HS_TOTAL_MS    10000u  /* full handshake is ~0.2-0.4 s wall    */
#endif
#ifndef CONN_REQ_TOTAL_MS
#define CONN_REQ_TOTAL_MS   10000u  /* ready -> complete request            */
#endif
#ifndef CONN_REQ_IDLE_MS
#define CONN_REQ_IDLE_MS    5000u   /* partial request, nothing new         */
#endif
#ifndef CONN_CLOSING_MS
#define CONN_CLOSING_MS     3000u   /* FIN exchange before close() forces it */
#endif

typedef enum {
    CONN_FREE = 0,
    CONN_TLS,
    CONN_WAIT_REQ,
    CONN_REQ,
    CONN_STREAM,
    CONN_CLOSING,
} conn_phase_t;

typedef enum {
    CONN_ACT_NONE = 0,
    CONN_ACT_TIMEOUT,       /* close it: the phase ran out (see reason)    */
    CONN_ACT_FORCE_CLOSE,   /* CLOSING ran out: close() the socket now     */
} conn_action_t;

typedef enum {
    CONN_CLOSE_DONE = 0,        /* response sent, Connection: close          */
    CONN_CLOSE_PARSE,           /* request rejected by the parser            */
    CONN_CLOSE_TIMEOUT_HS,      /* handshake ran out                         */
    CONN_CLOSE_TIMEOUT_REQ,     /* request total or no-progress ran out      */
    CONN_CLOSE_TLS,             /* TLS error / handshake failure             */
    CONN_CLOSE_TLS_REFUSED,     /* no TLS session free, or TLS not available */
    CONN_CLOSE_PEER,            /* peer FIN / RST                            */
    CONN_CLOSE_TAKEOVER,        /* stream handed to a newer request          */
    CONN_CLOSE_SEND,            /* send failed or stalled                    */
    CONN_CLOSE_POOL,            /* no request context free                   */
    CONN_CLOSE_REASONS_
} conn_close_reason_t;

typedef struct {
    uint8_t  phase;             /* conn_phase_t                              */
    int8_t   req;               /* pool index or -1                          */
    uint32_t t_phase;           /* when the current clock started            */
    uint32_t t_progress;        /* last request byte (REQ)                   */
} conn_t;

typedef struct {
    conn_t   c[CONN_MAX_SOCKETS];
    int8_t   req_owner[CONN_REQ_POOL];     /* socket index or -1            */
    uint8_t  req_in_use, req_peak;
    uint32_t closes[CONN_CLOSE_REASONS_];
    uint32_t forced;                        /* CLOSING ran out              */
} conn_table_t;

void          conn_init(conn_table_t *t);

/* Events. Return false when the event does not apply to the current phase
 * (the caller then knows its picture of the socket is off). */
bool          conn_on_established(conn_table_t *t, uint8_t sn, uint32_t now, bool tls);
bool          conn_on_tls_ready(conn_table_t *t, uint8_t sn, uint32_t now);
bool          conn_on_bytes(conn_table_t *t, uint8_t sn, uint32_t now);
bool          conn_on_stream(conn_table_t *t, uint8_t sn, uint32_t now);

/**
 * Begin closing. Idempotent: returns true only the first time for this
 * connection (the caller then does the TLS/socket teardown once); false if it
 * is already CLOSING or FREE. The request context is returned to the pool.
 */
bool          conn_close(conn_table_t *t, uint8_t sn, uint32_t now, conn_close_reason_t why);

/* The socket reached CLOSED (any path). Back to FREE, context returned. */
void          conn_on_closed(conn_table_t *t, uint8_t sn);

/* What the deadline says now. */
conn_action_t conn_tick(const conn_table_t *t, uint8_t sn, uint32_t now);
/* The close reason to use for a CONN_ACT_TIMEOUT in the current phase. */
conn_close_reason_t conn_timeout_reason(const conn_table_t *t, uint8_t sn);
/* CONN_ACT_FORCE_CLOSE was acted on. */
void          conn_note_forced(conn_table_t *t, uint8_t sn);

/* Request context index for this connection, or -1. */
int           conn_req(const conn_table_t *t, uint8_t sn);
conn_phase_t  conn_phase(const conn_table_t *t, uint8_t sn);
const char   *conn_phase_name(conn_phase_t p);
const char   *conn_reason_name(conn_close_reason_t r);

#endif /* CONN_H */
