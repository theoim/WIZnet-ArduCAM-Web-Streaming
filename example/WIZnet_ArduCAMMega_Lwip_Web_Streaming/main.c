/**
 * @file    main.c
 * @brief   ArduCAM MEGA -> WIZnet MACRAW + lwIP web streaming.
 *
 * The companion to the TOE example. Both serve the same page, the same
 * endpoints, and the same MJPEG stream from the same camera driver; the only
 * difference is where TCP is terminated.
 *
 *   TOE   the WIZnet chip runs the TCP/IP stack in hardware. The MCU reads and
 *         writes socket buffers.
 *   lwIP  the chip is put in MACRAW mode and passes raw Ethernet frames. lwIP
 *         runs the stack in software on the MCU.
 *
 * Run one board with each and put the two pages side by side: same camera, same
 * UI, and the frame-budget panel shows what the difference costs.
 *
 * The two use different IP addresses so they can share a network.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"
#include "w5x00_lwip.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "arducam_mega.h"
#include "httpd_stream.h"

/* --------------------------------- Config --------------------------------- */
#define PLL_SYS_KHZ     (200 * 1000)
#define SOCKET_MACRAW   0

#define FW_BUILD_TAG    "lwip " __DATE__ " " __TIME__

/* -------------------------------- Variables ------------------------------- */
static uint8_t   g_mac[6] = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x58};
static ip_addr_t g_ip;
static ip_addr_t g_mask;
static ip_addr_t g_gateway;

struct netif g_netif;

static uint8_t *g_frame_buf;

/**
 * Move everything the chip has received into lwIP, then run its timers.
 *
 * Drains the receive buffer rather than taking one frame per call: the capture
 * that follows blocks for tens of milliseconds, and anything still queued in
 * the chip during that window risks being overwritten. Left undrained, the lost
 * segments turn into retransmissions and the stream stutters.
 *
 * Also handed to the streaming code so it can keep the stack alive while it
 * waits for send-buffer space.
 */
static void net_service(void)
{
    for (;;) {
        uint16_t pack_len = 0;
        struct pbuf *p;

        getsockopt(SOCKET_MACRAW, SO_RECVBUF, &pack_len);
        if (pack_len == 0) {
            break;
        }

        pack_len = recv_lwip(SOCKET_MACRAW, g_frame_buf, pack_len);
        if (pack_len == 0) {
            break;
        }

        p = pbuf_alloc(PBUF_RAW, pack_len, PBUF_POOL);
        if (!p) {
            break;                       /* pool exhausted; drain again later */
        }

        pbuf_take(p, g_frame_buf, pack_len);
        if (g_netif.input(p, &g_netif) != ERR_OK) {
            pbuf_free(p);
        }
    }

    sys_check_timeouts();
}

/* ------------------------------- Platform --------------------------------- */
static void set_clock_khz(void)
{
    set_sys_clock_khz(PLL_SYS_KHZ, true);

    clock_configure(
        clk_peri,
        0,
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
        PLL_SYS_KHZ * 1000,
        PLL_SYS_KHZ * 1000
    );

    stdio_init_all();
}

/* ---------------------------------- Main ---------------------------------- */
int main(void)
{
    set_clock_khz();

    printf("Initializing ArduCAM MEGA...\n");
    arducam_mega.init();
    arducam_mega.set_pixel_format(PIXFORMAT_JPEG);
    arducam_mega.set_frame_size(g_resolution);

    printf("Initializing network (MACRAW + lwIP)...\n");
    sleep_ms(3000);

    wizchip_spi_initialize();
    wizchip_cris_initialize();
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    setSHAR(g_mac);
    ctlwizchip(CW_RESET_PHY, 0);

    IP4_ADDR(&g_ip,      192, 168, 11, 5);
    IP4_ADDR(&g_mask,    255, 255, 255, 0);
    IP4_ADDR(&g_gateway, 192, 168, 11, 1);

    lwip_init();

    netif_add(&g_netif, &g_ip, &g_mask, &g_gateway,
              NULL, netif_initialize, netif_input);
    g_netif.name[0] = 'e';
    g_netif.name[1] = '0';

    netif_set_link_callback(&g_netif, netif_link_callback);
    netif_set_status_callback(&g_netif, netif_status_callback);

    /* MACRAW hands raw Ethernet frames to lwIP instead of terminating TCP. */
    if (socket(SOCKET_MACRAW, Sn_MR_MACRAW, 0, 0x00) < 0) {
        printf("[ERR] MACRAW socket open failed\n");
        return -1;
    }

    netif_set_link_up(&g_netif);
    netif_set_up(&g_netif);

    g_frame_buf = malloc(ETHERNET_MTU);
    if (!g_frame_buf) {
        printf("[ERR] frame buffer allocation failed\n");
        return -1;
    }

    httpd_stream_init();
    httpd_stream_set_net_poll(net_service);

    printf("Firmware build: " FW_BUILD_TAG "\n");
    printf("HTTP camera server ready (lwIP)\n");
    printf("Open http://%s/ in a browser\n", ipaddr_ntoa(&g_ip));

    while (1) {
        net_service();
        httpd_stream_poll();
    }

    return 0;
}
