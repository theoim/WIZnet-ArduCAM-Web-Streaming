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
 *   web_page.h            the page
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
 *   GET /logo.png         logo, cached by the browser
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

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "exhibition_config.h"
#include "net_server.h"
#include "cam_state.h"
#include "cam_controls.h"
#include "ov2640.h"

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

    printf("OV2640 exhibition build (" STACK_BUILD_TAG ")\n");

    printf("Initializing OV2640...\n");
    camera.init();

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
    if (camera.set_frame_size(cam_state_resolution()) != 0) {
        printf("WARNING: set_frame_size(%s) failed - sensor may still be at "
               "the driver default\n",
               cam_state_res_string(cam_state_resolution()));
    }
    sleep_ms(200);

    /*
     * The control table is NOT pushed into the sensor at boot.
     *
     * Its ranges are provisional, and writing eleven unverified values into a
     * sensor before it has produced a single frame is a good way to end up with
     * a camera that answers HTTP and captures nothing. Controls are written
     * only when someone actually sets one; until then the sensor keeps whatever
     * its own initialisation gave it.
     *
     * The visible cost: the panel shows the table's defaults, not the sensor's
     * real values, until a control is touched.
     */

    printf("Initial resolution: %s\n",
           cam_state_res_string(cam_state_resolution()));

    cam_state_init();

#if EXHIBITION_AUTOSTART
    cam_state_set_streaming(true);
    printf("Streaming STARTED (autostart)\n");
#endif


    printf("Initializing network (" STACK_NAME ")...\n");
    net_server_init();


    while (1) {
        net_server_service();
    }

    return 0;
}
