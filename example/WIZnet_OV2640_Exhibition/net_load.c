/**
 * @file    net_load.c
 * @brief   Filler payload and byte accounting for the load generator.
 */

#include <string.h>

#include "net_load.h"
#include "exhibition_config.h"

/*
 * Printable filler rather than zeros. A zero-filled body compresses to nothing
 * if anything on the path decides to compress it, and then the load stops being
 * a load without saying so. Text that does not repeat within the block gives
 * the wire something real to carry.
 */
static uint8_t  g_chunk[LOAD_CHUNK_SIZE];
static bool     g_chunk_ready = false;

static volatile uint32_t g_bytes = 0;

const uint8_t *net_load_chunk(void)
{
    if (!g_chunk_ready) {
        uint32_t i;
        for (i = 0; i < LOAD_CHUNK_SIZE; i++) {
            /* 64 distinct printable characters, cycled with a stride so the
               block does not fall into a short repeating pattern. */
            g_chunk[i] = (uint8_t)('0' + ((i * 7u) % 64u));
        }
        g_chunk_ready = true;
    }
    return g_chunk;
}

uint32_t net_load_parse_kb(const char *q, size_t qlen)
{
    size_t i;

    for (i = 0; i + 3 <= qlen; i++) {
        if ((i == 0 || q[i - 1] == '&') &&
            q[i] == 'k' && q[i + 1] == 'b' && q[i + 2] == '=') {
            size_t   p     = i + 3;
            uint32_t value = 0;
            bool     digit = false;

            while (p < qlen && q[p] >= '0' && q[p] <= '9') {
                value = value * 10u + (uint32_t)(q[p] - '0');
                digit = true;
                p++;
                if (value > LOAD_KB_MAX) {
                    return LOAD_KB_MAX;   /* clamp rather than refuse */
                }
            }
            return digit ? value : 0u;
        }
    }
    return 0u;
}

void net_load_account(uint32_t bytes)
{
    g_bytes += bytes;
}

uint32_t net_load_take_bytes(void)
{
    uint32_t n = g_bytes;
    g_bytes = 0;
    return n;
}
