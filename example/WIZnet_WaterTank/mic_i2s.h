/**
 * @file    mic_i2s.h
 * @brief   INMP441 microphone on PIO I2S, running alongside the camera.
 *
 * The microphone is a second continuous DMA stream on a board whose camera
 * already depends on getting its own DMA armed inside the blanking interval
 * before VSYNC. That is the reason this module exists as something that can be
 * compiled out rather than as code wired straight into main: the question it
 * has to answer is not "does the microphone work" - that was settled in
 * WIZnet_I2S_Mic_Test - but "does the camera still hit its frame rate with the
 * microphone running".
 *
 * Set EXHIBITION_MIC to 0 to get the before half of that comparison.
 *
 * Pins are fixed by what the rest of the board has already taken:
 *
 *   GP2   BCLK    header pin 4
 *   GP3   WS      header pin 5     (must be BCLK + 1: PIO side-set is 2 bits)
 *   GP28  SD      header pin 34
 *
 * GP0/GP1 are the camera's SCCB bus, GP4 is its VSYNC, GP5..GP14 carry its
 * pixel data, PCLK and HREF, and GP15..GP22 belong to the W6300. GP26 is the
 * only pin left after this.
 */

#ifndef __MIC_I2S_H__
#define __MIC_I2S_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * Claim a PIO state machine and two DMA channels, then start capturing.
 *
 * Call this AFTER the camera has been initialised. The camera's sensor init
 * writes to GP27 once, and claiming PIO resources after it means this module
 * takes whatever the camera did not.
 *
 * @return true if the microphone is running.
 */
bool mic_i2s_init(void);

/**
 * Consume whatever the DMA has finished. Cheap and non-blocking - call it from
 * the main loop next to the network service.
 *
 * Doing nothing for a while is safe: the two buffers give about a quarter of a
 * second of slack before a frame is lost, and a lost frame is counted rather
 * than hidden.
 */
void mic_i2s_poll(void);

/** True once mic_i2s_init() has succeeded. */
bool mic_i2s_present(void);

/**
 * Level of the most recent completed window, left channel, DC removed.
 *
 * Both outputs are in the microphone's own 24-bit units, so full scale is
 * 8388607. Either pointer may be NULL.
 */
void mic_i2s_level(int32_t *peak, int32_t *rms);

/** Frames captured since boot, and buffers lost because poll() fell behind. */
uint32_t mic_i2s_frames(void);
uint32_t mic_i2s_overruns(void);

#endif /* __MIC_I2S_H__ */
