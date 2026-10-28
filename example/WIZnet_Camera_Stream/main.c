/**
 * @file    main.c
 * @brief   ArduCAM MEGA web streaming - exhibition build.
 *
 * Orchestration only. Bring the clock up, bring the camera up, hand over to
 * whichever server was compiled in, and loop. Everything with an opinion lives
 * elsewhere:
 *
 *   exhibition_config.h   which stack, which address, how big the load gets
 *   cam_state.c           capture state, the one-second window, the status JSON
 *   cam_controls.c        the sensor control table
 *   net_load.c            the load generator's payload and accounting
 *   web/index.html        the page (gzip'd into web_page_gz.h at build time)
 *   server_toe.c          hardware TCP/IP
 *   server_lwip.c         software TCP/IP
 *
 * The two firmware images differ by one -D. Flash both, put the boards side by
 * side, and every difference on screen came from the network stack rather than
 * from the camera, the page or the measurement.
 *
 * Endpoints
 *   GET /                 the page
 *   GET /stream           multipart/x-mixed-replace MJPEG
 *   GET /load?kb=N        N KB of filler - the load generator
 *   GET /api/start|stop   begin / end capture        -> status JSON
 *   GET /api/status       current state              -> status JSON
 *   GET /api/res?v=WxH    frame size                 -> status JSON
 *   GET /api/clk?div&pll  sensor clock dividers      -> status JSON
 *   GET /api/cam?<k>=<v>  any subset of the controls -> status JSON (409 if clamped)
 *   GET /api/controls     the control descriptors
 *   GET /api/reset        reinitialise a wedged sensor
 */

#include <stdio.h>

#include "build_stamp.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "stream_config.h"
#include "net_server.h"
#include "cam_state.h"
#include "cam_driver.h"
#include "dvp_capture.h"
#include "settings.h"
#if STREAM_PIPELINE
#include "cam_pipeline.h"
#endif
#include CAM_MODULE_HEADER

static void set_clock_khz(void)
{
    set_sys_clock_khz(PLL_SYS_KHZ, true);

#if defined(NET_STACK_LWIP)
    /*
     * The lwIP image retimes the peripheral clock to the system PLL, and it has
     * to happen here - before the camera is initialised. Moving the SPI clock
     * out from under an already-configured camera is how the sensor ends up
     * silent with nothing in the log to say why.
     *
     * The TOE image leaves the SDK default alone, as its example always has.
     */
    clock_configure(clk_peri, 0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    PLL_SYS_KHZ * 1000, PLL_SYS_KHZ * 1000);
#endif

    stdio_init_all();
    printf("System clock set to %d MHz\n", PLL_SYS_KHZ / 1000);
}

int main(void)
{
    set_clock_khz();

    printf("=== camera_stream (" STACK_BUILD_TAG ")  module: %s ===\n", CAM_MODULE_NAME);
    printf("build %s %s  driver %s\n", CAM_BUILD_GIT, CAM_BUILD_UTC, cam_caps.name);

    /* Before core 1 exists: the JEDEC ID read takes the flash out of XIP. */
    settings_boot();

    res_t saved_res;
    if (settings_boot_resolution(&saved_res)) {
        cam_state_set_boot_resolution(saved_res);
    }

    printf("Initializing camera...\n");
    cam_sensor_init();
    cam_capture_init();

    /*
     * The format is not written again; the size is.
     *
     * ov2640_sensor_init() already selects JPEG and a starting size, so the
     * pixel-format call the Mega needed here is gone - it would re-run the same
     * two hundred register writes to arrive where we already are. The size is
     * still applied, because the driver's starting size and cam_state's idea of
     * the current resolution are two separate defaults and this is where they
     * are made to agree. Changing cam_state's default is then enough.
     *
     * The Mega version needed 200 ms of settling between these writes or the
     * resolution was silently dropped. The OV2640 tables carry their own
     * settling inside set_framesize(). What survives from that episode is the
     * lesson rather than the code: the console reports our own state, so the
     * frame size in KB is the only honest check of what the sensor really did.
     */
    if (cam_set_framesize(cam_state_resolution()) != 0) {
        printf("WARNING: set_frame_size(%s) failed - sensor may still be at "
               "the driver default\n",
               cam_state_res_string(cam_state_resolution()));
    }
    sleep_ms(200);

    /*
     * No control is written at boot except what an admin saved. A saved value
     * was set on this sensor and seen working before Save was pressed; anything
     * else stays at the sensor's own initialisation.
     */

    printf("Initial resolution: %s\n",
           cam_state_res_string(cam_state_resolution()));

    cam_state_init();
    settings_apply_controls();          /* before core 1: written directly */

#if EXHIBITION_AUTOSTART
    cam_state_set_streaming(true);
    printf("Streaming STARTED (autostart)\n");
#endif


#if STREAM_PIPELINE
    /* The boot size write is the one the LH module was seen to drop: core 1
     * checks the first frames and writes it again if needed. */
    cam_pipeline_verify_res(cam_state_resolution());

    /* After the sensor is configured and before the network: from here on
     * core 1 owns the camera and core 0 only touches it through
     * cam_pipeline_pause(). */
    cam_pipeline_start();
#endif

#if CAM_QB_DIAG && STREAM_PIPELINE
    /*
     * Diagnostic image only: send empty control batches through the core 0 ->
     * core 1 slot and time the round trip. An empty batch writes nothing to
     * the sensor; this checks the hand-over itself (core 1 picks commands up
     * between frames) before any real control uses it.
     */
    sleep_ms(1000);                     /* let core 1 get into its capture loop */
    for (int i = 0; i < 5; i++) {
        cam_ctrl_batch_t  empty;
        cam_ctrl_result_t res = { 0, -1, 0 };
        cam_ctrl_batch_init(&empty);
        absolute_time_t t0 = get_absolute_time();
        cam_cmd_rc_t rc = cam_pipeline_set_controls(&empty, &res, CAM_CMD_TIMEOUT_MS);
        int64_t us = absolute_time_diff_us(t0, get_absolute_time());
        printf("[ctrl-diag] empty command %d: %s in %lld us (streaming %d)\n", i + 1,
               cam_cmd_rc_name(rc), (long long)us, cam_state_streaming() ? 1 : 0);
        sleep_ms(137);                  /* land at different points of the frame */
    }
#endif

    printf("Initializing network (" STACK_NAME ")...\n");
    net_server_init();


    while (1) {
        net_server_service();
    }

    return 0;
}
