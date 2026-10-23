/**
 * @file    conn_test.c
 * @brief   Host tests for http/conn.c with a fake millisecond clock.
 */

#include <stdio.h>
#include <string.h>

#include "conn.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    else g_pass++; } while (0)

static conn_table_t T;

static __attribute__((noinline)) void test_tls_path(void)
{
    conn_init(&T);
    CHECK(conn_on_established(&T, 0, 1000, true), "established fresh");
    CHECK(conn_phase(&T, 0) == CONN_TLS && conn_req(&T, 0) == -1, "TLS, no context yet");
    CHECK(conn_tick(&T, 0, 1000 + CONN_HS_TOTAL_MS) == CONN_ACT_NONE, "handshake at limit ok");
    CHECK(conn_tick(&T, 0, 1001 + CONN_HS_TOTAL_MS) == CONN_ACT_TIMEOUT, "handshake over limit");
    CHECK(conn_timeout_reason(&T, 0) == CONN_CLOSE_TIMEOUT_HS, "reason hs");

    CHECK(conn_on_tls_ready(&T, 0, 2000), "tls ready");
    CHECK(conn_phase(&T, 0) == CONN_WAIT_REQ && conn_req(&T, 0) == 0, "context 0 taken");
    CHECK(conn_tick(&T, 0, 2000 + CONN_REQ_TOTAL_MS + 1) == CONN_ACT_TIMEOUT, "no request in time");
    CHECK(conn_timeout_reason(&T, 0) == CONN_CLOSE_TIMEOUT_REQ, "reason req");
    CHECK(!conn_on_tls_ready(&T, 0, 2100), "tls ready twice rejected");
}

static __attribute__((noinline)) void test_request_clocks(void)
{
    conn_init(&T);
    conn_on_established(&T, 1, 0, true);
    conn_on_tls_ready(&T, 1, 100);
    /* first byte at 4 s: REQ, total still from 100 */
    CHECK(conn_on_bytes(&T, 1, 4100), "bytes");
    CHECK(conn_phase(&T, 1) == CONN_REQ, "REQ");
    /* no progress for IDLE: timeout */
    CHECK(conn_tick(&T, 1, 4100 + CONN_REQ_IDLE_MS) == CONN_ACT_NONE, "idle at limit");
    CHECK(conn_tick(&T, 1, 4101 + CONN_REQ_IDLE_MS) == CONN_ACT_TIMEOUT, "idle over limit");

    /* trickle: a byte every 1 s never trips idle but trips total */
    conn_init(&T);
    conn_on_established(&T, 1, 0, true);
    conn_on_tls_ready(&T, 1, 0);
    uint32_t now = 0;
    conn_action_t a = CONN_ACT_NONE;
    while (a == CONN_ACT_NONE && now < 60000) {
        now += 1000;
        a = conn_tick(&T, 1, now);
        if (a == CONN_ACT_NONE) conn_on_bytes(&T, 1, now);
    }
    CHECK(a == CONN_ACT_TIMEOUT && now == CONN_REQ_TOTAL_MS + 1000u - (CONN_REQ_TOTAL_MS % 1000u),
          "trickle stopped by total at %lu", (unsigned long)now);
    CHECK(now <= CONN_REQ_TOTAL_MS + 1000u, "total not extended by progress");
}

static __attribute__((noinline)) void test_wraparound(void)
{
    conn_init(&T);
    uint32_t start = 0xFFFFFFFFu - 2000u;          /* 2 s before the wrap */
    conn_on_established(&T, 2, start, true);
    CHECK(conn_tick(&T, 2, start + 5000u) == CONN_ACT_NONE, "5 s across wrap: within");
    CHECK(conn_tick(&T, 2, start + CONN_HS_TOTAL_MS + 1u) == CONN_ACT_TIMEOUT, "over limit across wrap");
    conn_on_tls_ready(&T, 2, 0xFFFFFFF0u);
    conn_on_bytes(&T, 2, 0xFFFFFFF0u);
    CHECK(conn_tick(&T, 2, 0x00000010u) == CONN_ACT_NONE, "32 ms across wrap");
    CHECK(conn_tick(&T, 2, 0xFFFFFFF0u + CONN_REQ_IDLE_MS + 1u) == CONN_ACT_TIMEOUT, "idle across wrap");
}

static __attribute__((noinline)) void test_stream_and_close(void)
{
    conn_init(&T);
    conn_on_established(&T, 3, 0, true);
    conn_on_tls_ready(&T, 3, 10);
    conn_on_bytes(&T, 3, 20);
    CHECK(conn_on_stream(&T, 3, 30), "stream");
    CHECK(conn_req(&T, 3) == -1 && T.req_in_use == 0, "context returned on stream");
    CHECK(conn_tick(&T, 3, 30 + 3600000u) == CONN_ACT_NONE, "stream: no idle timeout after 1 h");

    CHECK(conn_close(&T, 3, 5000, CONN_CLOSE_TAKEOVER), "first close acts");
    CHECK(!conn_close(&T, 3, 5001, CONN_CLOSE_PEER), "second close is a no-op");
    CHECK(T.closes[CONN_CLOSE_TAKEOVER] == 1 && T.closes[CONN_CLOSE_PEER] == 0, "counted once");
    CHECK(conn_tick(&T, 3, 5000 + CONN_CLOSING_MS) == CONN_ACT_NONE, "closing at limit");
    CHECK(conn_tick(&T, 3, 5001 + CONN_CLOSING_MS) == CONN_ACT_FORCE_CLOSE, "closing over limit");
    conn_note_forced(&T, 3);
    conn_on_closed(&T, 3);
    CHECK(conn_phase(&T, 3) == CONN_FREE && T.forced == 1, "back to FREE after forced close");
    CHECK(!conn_close(&T, 3, 9000, CONN_CLOSE_PEER), "close on FREE is a no-op");
    /* listen recovery: a new connection on the same socket */
    CHECK(conn_on_established(&T, 3, 10000, true) && conn_phase(&T, 3) == CONN_TLS, "socket reused");
}

static __attribute__((noinline)) void test_pool(void)
{
    conn_init(&T);
    /* plaintext: context taken at ESTABLISHED; 4 fit, the 5th does not */
    for (uint8_t s = 0; s < CONN_REQ_POOL; s++) {
        CHECK(conn_on_established(&T, s, 0, false), "plain %u", s);
        CHECK(conn_req(&T, s) == s, "context %u", s);
    }
    CHECK(!conn_on_established(&T, 4, 0, false), "5th: pool full");
    CHECK(conn_req(&T, 4) == -1 && T.req_peak == CONN_REQ_POOL, "no context, peak %u", T.req_peak);
    CHECK(conn_close(&T, 4, 1, CONN_CLOSE_POOL), "close pool-full conn");
    CHECK(T.closes[CONN_CLOSE_POOL] == 1, "pool close counted");

    /* return one, the next connection gets it */
    conn_close(&T, 1, 2, CONN_CLOSE_DONE);
    conn_on_closed(&T, 1);
    CHECK(T.req_in_use == CONN_REQ_POOL - 1, "returned");
    CHECK(conn_on_established(&T, 5, 3, false) && conn_req(&T, 5) == 1, "reused context 1");

    /* double release is harmless */
    conn_on_closed(&T, 5);
    conn_on_closed(&T, 5);
    CHECK(T.req_in_use == CONN_REQ_POOL - 1, "double closed does not underflow (%u)", T.req_in_use);

    /* missed CLOSED: ESTABLISHED on a socket we still think is busy */
    conn_init(&T);
    conn_on_established(&T, 6, 0, false);
    CHECK(!conn_on_established(&T, 6, 100, false), "stale state reported");
    CHECK(T.req_in_use == 1 && conn_req(&T, 6) >= 0, "no context leaked (%u)", T.req_in_use);

    /* invalid socket numbers */
    CHECK(!conn_on_established(&T, CONN_MAX_SOCKETS, 0, true), "sn out of range");
    CHECK(conn_tick(&T, 200, 0) == CONN_ACT_NONE && conn_req(&T, 200) == -1, "out of range is inert");
}

static __attribute__((noinline)) void test_events_out_of_phase(void)
{
    conn_init(&T);
    CHECK(!conn_on_bytes(&T, 0, 0), "bytes on FREE");
    CHECK(!conn_on_stream(&T, 0, 0), "stream on FREE");
    conn_on_established(&T, 0, 0, true);
    CHECK(!conn_on_bytes(&T, 0, 0), "bytes during TLS");
    CHECK(!conn_on_stream(&T, 0, 0), "stream during TLS");
    conn_on_tls_ready(&T, 0, 0);
    conn_on_stream(&T, 0, 0);
    CHECK(!conn_on_bytes(&T, 0, 1), "bytes on STREAM not tracked");
    CHECK(strcmp(conn_phase_name(CONN_STREAM), "stream") == 0, "phase name");
    CHECK(strcmp(conn_reason_name(CONN_CLOSE_TIMEOUT_HS), "timeout-handshake") == 0, "reason name");
}

int main(void)
{
    printf("conn host tests (HS %u, REQ %u, IDLE %u, CLOSING %u ms, pool %d, sizeof(conn_table_t)=%u)\n",
           CONN_HS_TOTAL_MS, CONN_REQ_TOTAL_MS, CONN_REQ_IDLE_MS, CONN_CLOSING_MS, CONN_REQ_POOL,
           (unsigned)sizeof(conn_table_t));
    test_tls_path();
    test_request_clocks();
    test_wraparound();
    test_stream_and_close();
    test_pool();
    test_events_out_of_phase();
    printf("%s: %d checks passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
