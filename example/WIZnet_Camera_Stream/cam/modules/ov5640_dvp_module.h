/**
 * @file    ov5640_dvp_module.h
 * @brief   Pin map: OV5640 DVP module with on-board oscillator, exhibition wiring.
 *
 * The module carries its own oscillator (checked on the board, 2026-10-21), so
 * the MCU drives no clock. The sensor still needs to know the frequency: the
 * OV5640 PLL settings are computed from it, and a wrong value shows up as the
 * wrong frame rate and PCLK rather than as an error. Confirm the marking on the
 * oscillator can and correct CAM_XCLK_HZ before tuning anything.
 */

#ifndef CAM_MODULE_OV5640_DVP_MODULE_H
#define CAM_MODULE_OV5640_DVP_MODULE_H

#define CAM_MODULE_NAME      "OV5640 DVP module (on-board oscillator)"

#define CAM_PIN_SDA          0
#define CAM_PIN_SCL          1
#define CAM_PIN_VSYNC        4
#define CAM_PIN_D0           5
#define CAM_PIN_PWDN         27
#define CAM_PIN_RESET        (-1)

#define CAM_PIN_XCLK         (-1)                 /* on-module oscillator */
#define CAM_XCLK_HZ          (24 * 1000 * 1000)   /* TO CONFIRM from the can marking */

#define CAM_SCCB_HZ          (100 * 1000)

#endif
