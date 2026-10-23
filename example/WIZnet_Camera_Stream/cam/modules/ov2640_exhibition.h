/**
 * @file    ov2640_exhibition.h
 * @brief   Pin map: bare OV2640 DVP module on W6300-EVB-Pico2 (exhibition build).
 *
 * Same DVP wiring as the Arducam module. The OV2640 module has no oscillator,
 * so XCLK is hand-soldered to connector pin 8 and driven from GP26 by PWM
 * (20 MHz divides exactly out of a 200 MHz system clock).
 */

#ifndef CAM_MODULE_OV2640_EXHIBITION_H
#define CAM_MODULE_OV2640_EXHIBITION_H

#define CAM_MODULE_NAME      "OV2640 DVP module (exhibition wiring)"

#define CAM_PIN_SDA          0
#define CAM_PIN_SCL          1
#define CAM_PIN_VSYNC        4
#define CAM_PIN_D0           5
#define CAM_PIN_PWDN         27
#define CAM_PIN_RESET        (-1)    /* GP28 once soldered */

#define CAM_PIN_XCLK         26
#define CAM_XCLK_HZ          (20 * 1000 * 1000)

#define CAM_SCCB_HZ          (200 * 1000)

#endif
