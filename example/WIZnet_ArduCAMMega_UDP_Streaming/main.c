#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_spi.h"
#include "socket.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "arducam_mega.h"

#define PLL_SYS_KHZ (200 * 1000)
#define HEADER_SIZE 4
#define MAX_UDP_PAYLOAD 1400
#define PAYLOAD_SIZE (MAX_UDP_PAYLOAD - HEADER_SIZE)
#define ETHERNET_BUF_SIZE (1024 * 4)
#define UDP_SOCKET 0
#define PORT_STREAM 5000
#define MAX_JPEG_SIZE (200 * 1024)
#define TX_MARGIN_BYTES 16
#define PACE_EVERY_N_PKTS 8
#define PACE_SLEEP_US 100

#if _WIZCHIP_ >= W6100
    #define SOCK_MODE Sn_MR_UDP4
    #define SOCK_FLAG 0
#else
    #define SOCK_MODE Sn_MR_UDP
    #define SOCK_FLAG 0
#endif

/* ---------------------------- External (ArduCAM) --------------------------- */
extern uint8_t image_buff[MAX_JPEG_SIZE];  // ArduCAM image buffer (120KB)

/* -------------------------------- Variables ------------------------------- */
/* Network */
static wiz_NetInfo g_net_info =
{
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x57},
    .ip  = {192, 168, 11, 3},
    .sn  = {255, 255, 255, 0},
    .gw  = {192, 168, 11, 1},
    .dns = {8, 8, 8, 8},
#if _WIZCHIP_ > W5500
    .lla = {0xfe, 0x80, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x02, 0x08, 0xdc, 0xff,
            0xfe, 0x57, 0x57, 0x26},
    .gua = {0},
    .sn6 = {0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00},
    .gw6 = {0},
    .dns6 = {0x20, 0x01, 0x48, 0x60,
             0x48, 0x60, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x88, 0x88},
    .ipmode = NETINFO_STATIC_ALL
#else
    .dhcp = NETINFO_STATIC
#endif
};

static uint8_t g_ethernet_buf[ETHERNET_BUF_SIZE] = {0};
static uint8_t tx_packet[MAX_UDP_PAYLOAD];

volatile bool streaming_active = false;
volatile uint8_t frame_id = 0;
volatile res_t current_resolution = RES_320X240;


/* --------------------------- Function Prototypes --------------------------- */
static res_t parse_resolution_command(const char* cmd);
static const char* get_resolution_string(res_t res);
static void set_clock_khz(void);
static void network_init(void);

static int32_t send_jpeg_frame(uint8_t socket, uint8_t *jpeg_data, unsigned long jpeg_size);
static int32_t udp_streaming_handler(uint8_t socket, uint16_t port);

int main(void)
{
    int32_t ret;

    set_clock_khz();

    printf("Initializing ArduCAM MEGA...\n");
    arducam_mega.init();

    arducam_mega.set_pixel_format(PIXFORMAT_JPEG);
    arducam_mega.set_frame_size(current_resolution);
    printf("Initial resolution: %s\n", get_resolution_string(current_resolution));

    printf("Initializing network...\n");
    network_init();

    printf("UDP JPEG streaming server ready\n");
    printf("Send 'START' to begin streaming, 'STOP' to end\n");

#if _WIZCHIP_ >= W6100
    printf("Running on W6x00 (IPv4/IPv6 capable) - Using IPv4 UDP mode\n");
#else
    printf("Running on W5x00 - Using standard UDP mode\n");
#endif

    while (1)
    {
        ret = udp_streaming_handler(UDP_SOCKET, PORT_STREAM);
        if (ret < 0) {
            printf("UDP handler error: %d\n", ret);
            sleep_ms(1000);
        }
    }

    return 0;
}

static res_t parse_resolution_command(const char* cmd) {
    if (strncmp(cmd, "RES_320X240", 11) == 0) return RES_320X240;
    if (strncmp(cmd, "RES_640X480", 11) == 0) return RES_640X480;
    if (strncmp(cmd, "RES_1280X720", 12) == 0) return RES_1280X720;
    if (strncmp(cmd, "RES_1600X1200", 13) == 0) return RES_1600X1200;
    if (strncmp(cmd, "RES_1920X1080", 13) == 0) return RES_1920X1080;
    return RES_320X240;
}

static const char* get_resolution_string(res_t res) {
    switch(res) {
        case RES_320X240:  return "320x240";
        case RES_640X480:  return "640x480";
        case RES_1280X720: return "1280x720";
        case RES_1600X1200:return "1600x1200";
        case RES_1920X1080:return "1920x1080";
        default:           return "Unknown";
    }
}

static void set_clock_khz(void)
{
    set_sys_clock_khz(PLL_SYS_KHZ, true);
    stdio_init_all();
    printf("System clock set to %d MHz\n", PLL_SYS_KHZ / 1000);
}

static void network_init(void)
{
    sleep_ms(3000);

    wizchip_spi_initialize();
    printf("WIZnet UDP JPEG Streaming (ArduCAM MEGA)\r\n");
    wizchip_cris_initialize();

    wizchip_reset();
    wizchip_initialize();
    wizchip_check();

    network_initialize(g_net_info);
    print_network_information(g_net_info);
}

static int32_t send_jpeg_frame(uint8_t socket, uint8_t *jpeg_data, unsigned long jpeg_size)
{
    uint8_t destip[4] = {192, 168, 11, 4};
    uint16_t destport = 5000;
    uint8_t dest_len = 4;
    int32_t ret;
    uint8_t total_packets;
    uint8_t pkt_id;
    unsigned long offset;
    unsigned long remain;
    uint16_t chunk_size;

    if (jpeg_size < 2 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        printf("[WARN] Frame %u invalid JPEG header (%02X %02X)\n",
               frame_id, jpeg_data[0], jpeg_data[1]);
        return -1;
    }

    total_packets = (jpeg_size + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
    if (total_packets == 0 || total_packets > 255) {
        printf("Invalid total_packets=%u (size=%lu)\n", total_packets, jpeg_size);
        return -2;
    }

    for (pkt_id = 0; pkt_id < total_packets; pkt_id++) {
        offset = (unsigned long)pkt_id * PAYLOAD_SIZE;
        remain = jpeg_size - offset;
        chunk_size = (remain > PAYLOAD_SIZE) ? PAYLOAD_SIZE : (uint16_t)remain;

        tx_packet[0] = frame_id;
        tx_packet[1] = pkt_id;
        tx_packet[2] = total_packets;
        tx_packet[3] = (pkt_id == total_packets - 1) ? 0x01 : 0x00;

        memcpy(tx_packet + HEADER_SIZE, &jpeg_data[offset], chunk_size);

        uint16_t tx_free = 0;
        do {
            getsockopt(socket, SO_SENDBUF, &tx_free);
            if (tx_free < (chunk_size + HEADER_SIZE + TX_MARGIN_BYTES)) {
                // sleep_us(10);
            }
        } while (tx_free < (chunk_size + HEADER_SIZE + TX_MARGIN_BYTES));

        if ((pkt_id % PACE_EVERY_N_PKTS) == 0) {
            // sleep_us(PACE_SLEEP_US);
        }

        ret = sendto(socket, tx_packet, chunk_size + HEADER_SIZE, destip, destport, dest_len);
        if (ret < 0) {
            return ret;
        }
    }

    return 1;
}

/**
 * @brief Handle UDP streaming socket
 */
static int32_t udp_streaming_handler(uint8_t sn, uint16_t port)
{
    int32_t  ret;
    uint8_t  destip[4];
    uint16_t destport;
    uint8_t  dest_len;

    unsigned long jpeg_size;
    int       capture_result;
    uint8_t   status;
    uint16_t  received_size;

    uint16_t  tx_free;
    static uint32_t frame_count = 0;
    static uint32_t error_count = 0;

    getsockopt(sn, SO_STATUS, &status);

    switch (getSn_SR(sn))
    {
    case SOCK_UDP:
        // Handle incoming commands
        getsockopt(sn, SO_RECVBUF, &received_size);
        if (received_size)
        {
            if (received_size > ETHERNET_BUF_SIZE) received_size = ETHERNET_BUF_SIZE;

            ret = recvfrom(sn, g_ethernet_buf, received_size, destip, &destport, &dest_len);
            if (ret <= 0) return ret;

            received_size = (uint16_t)ret;

            // Parse START command
            if (received_size == 5 && !memcmp(g_ethernet_buf, "START", 5))
            {
                streaming_active = true;
                frame_id = 0;
                frame_count = 0;
                error_count = 0;
                printf("Streaming STARTED\n");
            }
            // Parse STOP command
            else if (received_size == 4 && !memcmp(g_ethernet_buf, "STOP", 4))
            {
                streaming_active = false;
                printf("Streaming STOPPED (frames=%u, errors=%u)\n", frame_count, error_count);
            }
            // Parse resolution change command
            else if (received_size >= 9 && !memcmp(g_ethernet_buf, "RES_", 4))
            {
                char res_cmd[20] = {0};
                int cmd_len = (ret > 19) ? 19 : ret;
                memcpy(res_cmd, g_ethernet_buf, cmd_len);
                res_cmd[cmd_len] = '\0';

                res_t new_res = parse_resolution_command(res_cmd);
                if (new_res != current_resolution) {
                    current_resolution = new_res;
                    printf("Resolution changed to %s\n", get_resolution_string(current_resolution));

                    // Apply new resolution
                    arducam_mega.set_frame_size(current_resolution);
                    sleep_ms(200);  // Allow camera to stabilize

                    // Send acknowledgment
                    const char* ack_msg = "RES_OK";
                    sendto(sn, (uint8_t*)ack_msg, strlen(ack_msg), destip, destport, dest_len);
                }
            }
        }

        // Streaming loop
        if (streaming_active) {
            // Check TX buffer availability before capturing
            getsockopt(sn, SO_SENDBUF, &tx_free);

            frame_id++;
            frame_count++;
            // Capture JPEG frame
            jpeg_size = 0;
            capture_result = arducam_mega.get_frame();

            if (capture_result == 0) {
                // Success: get frame size
                jpeg_size = arducam_mega.frame.frame_length;

                // Send frame over UDP
                ret = send_jpeg_frame(sn, image_buff, jpeg_size);
            }

            
            // Small delay to prevent overwhelming the system
            sleep_us(1);
        }
        break;

    case SOCK_CLOSED:
        printf("Socket closed, reopening...\n");
        streaming_active = false;  // Stop streaming on socket close
        {
            int sock_ret = socket(sn, SOCK_MODE, port, SOCK_FLAG);
            if (sock_ret < 0) {
                printf("Socket open failed: %d\n", sock_ret);
                return sock_ret;
            }
            printf("Socket reopened (sn=%d, mode=0x%02X, flag=0x%02X)\n", 
                   sock_ret, SOCK_MODE, SOCK_FLAG);
        }
        break;

    default:
        printf("Unknown socket status: 0x%02X\n", getSn_SR(sn));
        sleep_ms(100);
        break;
    }

    return 1;
}