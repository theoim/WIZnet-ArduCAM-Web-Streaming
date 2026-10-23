/**
 * @file    net_load.h
 * @brief   The load generator - the exhibit.
 *
 * GET /load?kb=N answers N KB of filler. The page asks for it again as soon as
 * the last one lands, on ONE connection, while the stream keeps running. That
 * puts real TCP work on the device with nothing but a browser in the room.
 *
 * What it shows: on the lwIP board those bytes are the MCU's work, so the frame
 * rate falls; on the TOE board the chip carries them, so it falls less. Both
 * boards use one extra socket, so the difference cannot be read as one of them
 * running out of sockets - see the note in exhibition_config.h.
 *
 * The device never learns the slider position. It counts the bytes it actually
 * served in the last second and reports that, which is the honest number: if
 * the browser cannot keep the load up, the reported rate falls with it.
 */

#ifndef __NET_LOAD_H__
#define __NET_LOAD_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Filler block, LOAD_CHUNK_SIZE bytes, constant for the life of the program. */
const uint8_t *net_load_chunk(void);

/**
 * Parse "kb=N" out of a query slice and clamp it to LOAD_KB_MAX.
 * @return requested size in KB, or 0 when the key is absent or unusable.
 */
uint32_t net_load_parse_kb(const char *q, size_t qlen);

/** Count bytes handed to the peer. Called by whichever server served them. */
void     net_load_account(uint32_t bytes);

/** Bytes served since the last call, and reset. Used to roll the 1 s window. */
uint32_t net_load_take_bytes(void);

#endif /* __NET_LOAD_H__ */
