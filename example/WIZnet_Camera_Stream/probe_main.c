/**
 * @file    probe_main.c
 * @brief   camera_probe: power the camera, identify it, print over USB. No network.
 *
 * The first image of the camera library, and the one to flash whenever a module
 * is swapped: it answers "which part is on this connector" before any driver
 * assumes an answer. It never touches the W6300, so it can run on a board that
 * is not on the LAN.
 *
 * The report repeats every three seconds, so a terminal opened late still sees
 * it.
 */

#include <stdio.h>

#include "build_stamp.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#include "cam_sccb.h"
#include "cam_probe.h"
#include CAM_MODULE_HEADER

static void xclk_start(void)
{
#if CAM_PIN_XCLK >= 0
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t top = sys_hz / CAM_XCLK_HZ - 1u;
    gpio_set_function(CAM_PIN_XCLK, GPIO_FUNC_PWM);
    gpio_set_drive_strength(CAM_PIN_XCLK, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_slew_rate(CAM_PIN_XCLK, GPIO_SLEW_RATE_FAST);
    uint slice = pwm_gpio_to_slice_num(CAM_PIN_XCLK);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, top);
    pwm_init(slice, &c, false);
    pwm_set_chan_level(slice, pwm_gpio_to_channel(CAM_PIN_XCLK), (top + 1u) / 2u);
    pwm_set_enabled(slice, true);
    printf("XCLK: GP%d at %lu Hz\n", CAM_PIN_XCLK, (unsigned long)(sys_hz / (top + 1u)));
#else
    printf("XCLK: none driven (module oscillator)\n");
#endif
}

int main(void)
{
    /* The streaming builds run at 200 MHz; match it so XCLK divides the same. */
    set_sys_clock_khz(200000, true);
    stdio_init_all();

    for (int i = 0; i < 50 && !stdio_usb_connected(); i++)
        sleep_ms(100);

    printf("\n=== camera_probe  module: %s ===\n", CAM_MODULE_NAME);
    printf("build %s %s\n", CAM_BUILD_GIT, CAM_BUILD_UTC);

    /* PWDN is active high. Claim the pin and drive it - the inherited driver
     * wrote it without setting a direction, which did nothing. */
    gpio_init(CAM_PIN_PWDN);
    gpio_set_dir(CAM_PIN_PWDN, GPIO_OUT);
    gpio_put(CAM_PIN_PWDN, 0);
#if CAM_PIN_RESET >= 0
    gpio_init(CAM_PIN_RESET);
    gpio_set_dir(CAM_PIN_RESET, GPIO_OUT);
    gpio_put(CAM_PIN_RESET, 1);
#endif
    xclk_start();

    /* The Quick-Bootup module claims 300 ms to boot; give it more. */
    sleep_ms(500);

    sccb_init(i2c0, CAM_PIN_SDA, CAM_PIN_SCL, CAM_SCCB_HZ);
    printf("SCCB: i2c0 SDA GP%d SCL GP%d at %u Hz, PWDN GP%d low\n",
           CAM_PIN_SDA, CAM_PIN_SCL, (unsigned)CAM_SCCB_HZ, CAM_PIN_PWDN);

    for (unsigned n = 1;; n++) {
        printf("\n--- probe #%u at %lu ms ---\n", n,
               (unsigned long)to_ms_since_boot(get_absolute_time()));
        cam_probe_report();
        sleep_ms(3000);
    }
}
