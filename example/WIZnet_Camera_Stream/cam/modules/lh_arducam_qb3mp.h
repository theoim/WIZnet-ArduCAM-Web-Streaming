/**
 * @file    lh_arducam_qb3mp.h
 * @brief   Pin map: Arducam Quick-Bootup 3MP DVP (B0554) on W6300-EVB-Pico2.
 *
 * The camera used in the LH water tank build. Wiring is the exhibition DVP
 * wiring (GP0/1 SCCB, GP4 VSYNC, GP5-12 D0-D7, GP13 PCLK, GP14 HREF).
 *
 * The module has its own oscillator and its own controller: the host talks to
 * the controller at SCCB 0x1F with 16-bit register addresses, never to the
 * image sensor behind it (whose part number Arducam does not publish).
 */

#ifndef CAM_MODULE_LH_ARDUCAM_QB3MP_H
#define CAM_MODULE_LH_ARDUCAM_QB3MP_H

#define CAM_MODULE_NAME      "Arducam Quick-Bootup 3MP DVP (B0554)"

#define CAM_PIN_SDA          0
#define CAM_PIN_SCL          1
#define CAM_PIN_VSYNC        4
#define CAM_PIN_D0           5       /* D0..D7 = GP5..GP12, PCLK GP13, HREF GP14 */
#define CAM_PIN_PWDN         27      /* active high: low = running */
#define CAM_PIN_RESET        (-1)    /* not wired */

#define CAM_PIN_XCLK         (-1)    /* on-module oscillator: the MCU drives no clock */
#define CAM_XCLK_HZ          0       /* unknown and not needed: the controller owns it */

#define CAM_SCCB_HZ          (200 * 1000)

#endif
