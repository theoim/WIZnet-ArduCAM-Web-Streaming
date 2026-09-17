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
#include "arducam_mega.h"

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

    printf("ArduCAM MEGA exhibition build (" STACK_BUILD_TAG ")\n");

    printf("Initializing ArduCAM MEGA...\n");
    arducam_mega.init();

    /*
     * init() leaves the sensor at QVGA - it sets JPEG and 320x240 itself, with
     * a 200 ms settle between and after. Overriding it needs the same timing:
     * the first version called set_pixel_format() and set_frame_size() back to
     * back with no wait and the resolution write was silently dropped. The
     * console then printed "Initial resolution: 1280x720" while the sensor was
     * still sending 4 KB QVGA frames, because that line reports our own state
     * and nobody checked what the sensor did with the write.
     *
     * The return value is checked now for the same reason. set_frame_size()
     * reports a failed I2C write and the old code threw it away.
     */
    sleep_ms(200);
    arducam_mega.set_pixel_format(PIXFORMAT_JPEG);
    sleep_ms(200);

    if (arducam_mega.set_frame_size(cam_state_resolution()) != 0) {
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
