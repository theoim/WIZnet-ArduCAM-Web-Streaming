/**
 * @file    conn.c
 * @brief   Connection lifetime - see conn.h.
 */

#include <string.h>

#include "conn.h"

static bool elapsed(uint32_t now, uint32_t then, uint32_t limit)
{
    return (uint32_t)(now - then) > limit;      /* wrap-safe for limit < 2^31 */
}

static bool valid(uint8_t sn) { return sn < CONN_MAX_SOCKETS; }

static void req_release(conn_table_t *t, uint8_t sn)
{
    int8_t i = t->c[sn].req;
    if (i >= 0 && i < CONN_REQ_POOL && t->req_owner[i] == (int8_t)sn) {
        t->req_owner[i] = -1;
        t->req_in_use--;
    }
    t->c[sn].req = -1;
}

static bool req_acquire(conn_table_t *t, uint8_t sn)
{
    if (t->c[sn].req >= 0) return true;
    for (int8_t i = 0; i < CONN_REQ_POOL; i++) {
        if (t->req_owner[i] < 0) {
            t->req_owner[i] = (int8_t)sn;
            t->c[sn].req = i;
            t->req_in_use++;
            if (t->req_in_use > t->req_peak) t->req_peak = t->req_in_use;
            return true;
        }
    }
    return false;
}

void conn_init(conn_table_t *t)
{
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < CONN_MAX_SOCKETS; i++) t->c[i].req = -1;
    for (int i = 0; i < CONN_REQ_POOL; i++)    t->req_owner[i] = -1;
}

/* A fresh connection on a socket that we thought was FREE. If it was anything
 * else the old state is stale (missed CLOSED): clean it first. */
bool conn_on_established(conn_table_t *t, uint8_t sn, uint32_t now, bool tls)
{
    if (!valid(sn)) return false;
    conn_t *c = &t->c[sn];
    bool fresh = (c->phase == CONN_FREE);
    if (!fresh) req_release(t, sn);
    c->t_phase = c->t_progress = now;
    if (tls) {
        c->phase = CONN_TLS;
        return fresh;
    }
    c->phase = CONN_WAIT_REQ;
    if (!req_acquire(t, sn)) return false;      /* caller closes with CONN_CLOSE_POOL */
    return fresh;
}

bool conn_on_tls_ready(conn_table_t *t, uint8_t sn, uint32_t now)
{
    if (!valid(sn) || t->c[sn].phase != CONN_TLS) return false;
    t->c[sn].phase = CONN_WAIT_REQ;
    t->c[sn].t_phase = t->c[sn].t_progress = now;
    return req_acquire(t, sn);                  /* false -> caller closes with CONN_CLOSE_POOL */
}

bool conn_on_bytes(conn_table_t *t, uint8_t sn, uint32_t now)
{
    if (!valid(sn)) return false;
    conn_t *c = &t->c[sn];
    if (c->phase == CONN_WAIT_REQ) c->phase = CONN_REQ;     /* total keeps running */
    if (c->phase != CONN_REQ) return false;
    c->t_progress = now;
    return true;
}

bool conn_on_stream(conn_table_t *t, uint8_t sn, uint32_t now)
{
    if (!valid(sn)) return false;
    conn_t *c = &t->c[sn];
    if (c->phase != CONN_REQ && c->phase != CONN_WAIT_REQ) return false;
    req_release(t, sn);
    c->phase = CONN_STREAM;
    c->t_phase = c->t_progress = now;
    return true;
}

bool conn_close(conn_table_t *t, uint8_t sn, uint32_t now, conn_close_reason_t why)
{
    if (!valid(sn)) return false;
    conn_t *c = &t->c[sn];
    if (c->phase == CONN_FREE || c->phase == CONN_CLOSING) return false;
    req_release(t, sn);
    if ((unsigned)why < CONN_CLOSE_REASONS_) t->closes[why]++;
    c->phase = CONN_CLOSING;
    c->t_phase = c->t_progress = now;
    return true;
}

void conn_on_closed(conn_table_t *t, uint8_t sn)
{
    if (!valid(sn)) return;
    req_release(t, sn);
    t->c[sn].phase = CONN_FREE;
}

conn_action_t conn_tick(const conn_table_t *t, uint8_t sn, uint32_t now)
{
    if (!valid(sn)) return CONN_ACT_NONE;
    const conn_t *c = &t->c[sn];
    switch ((conn_phase_t)c->phase) {
    case CONN_TLS:
        return elapsed(now, c->t_phase, CONN_HS_TOTAL_MS) ? CONN_ACT_TIMEOUT : CONN_ACT_NONE;
    case CONN_WAIT_REQ:
        return elapsed(now, c->t_phase, CONN_REQ_TOTAL_MS) ? CONN_ACT_TIMEOUT : CONN_ACT_NONE;
    case CONN_REQ:
        if (elapsed(now, c->t_phase, CONN_REQ_TOTAL_MS))   return CONN_ACT_TIMEOUT;
        if (elapsed(now, c->t_progress, CONN_REQ_IDLE_MS)) return CONN_ACT_TIMEOUT;
        return CONN_ACT_NONE;
    case CONN_CLOSING:
        return elapsed(now, c->t_phase, CONN_CLOSING_MS) ? CONN_ACT_FORCE_CLOSE : CONN_ACT_NONE;
    case CONN_FREE:
    case CONN_STREAM:
    default:
        return CONN_ACT_NONE;
    }
}

conn_close_reason_t conn_timeout_reason(const conn_table_t *t, uint8_t sn)
{
    if (valid(sn) && t->c[sn].phase == CONN_TLS) return CONN_CLOSE_TIMEOUT_HS;
    return CONN_CLOSE_TIMEOUT_REQ;
}

void conn_note_forced(conn_table_t *t, uint8_t sn)
{
    (void)sn;
    t->forced++;
}

int conn_req(const conn_table_t *t, uint8_t sn)
{
    return valid(sn) ? t->c[sn].req : -1;
}

conn_phase_t conn_phase(const conn_table_t *t, uint8_t sn)
{
    return valid(sn) ? (conn_phase_t)t->c[sn].phase : CONN_FREE;
}

const char *conn_phase_name(conn_phase_t p)
{
    switch (p) {
    case CONN_FREE:     return "free";
    case CONN_TLS:      return "tls";
    case CONN_WAIT_REQ: return "wait-req";
    case CONN_REQ:      return "req";
    case CONN_STREAM:   return "stream";
    case CONN_CLOSING:  return "closing";
    default:            return "?";
    }
}

const char *conn_reason_name(conn_close_reason_t r)
{
    switch (r) {
    case CONN_CLOSE_DONE:        return "done";
    case CONN_CLOSE_PARSE:       return "parse";
    case CONN_CLOSE_TIMEOUT_HS:  return "timeout-handshake";
    case CONN_CLOSE_TIMEOUT_REQ: return "timeout-request";
    case CONN_CLOSE_TLS:         return "tls";
    case CONN_CLOSE_TLS_REFUSED: return "tls-refused";
    case CONN_CLOSE_PEER:        return "peer";
    case CONN_CLOSE_TAKEOVER:    return "takeover";
    case CONN_CLOSE_SEND:        return "send";
    case CONN_CLOSE_POOL:        return "pool";
    default:                     return "?";
    }
}
