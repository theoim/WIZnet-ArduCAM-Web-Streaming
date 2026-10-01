/**
 * wiz_claw_net.c — ioLibrary DNS / DHCP 래퍼
 */
#include "wiz_claw_net.h"

#include "dns.h"
#include "dhcp.h"
#include "pico/stdlib.h"  /* sleep_ms */

static const char *TAG = "wiz_claw_net";

/* ── DNS 내부 상태 ───────────────────────────────────────────── */
static uint8_t  s_dns_buf[MAX_DNS_BUF_SIZE];
static uint8_t  s_dns_server_ip[4];
static uint8_t  s_dns_socket;

/* ── DHCP 내부 상태 ──────────────────────────────────────────── */
static uint8_t  s_dhcp_buf[548];   /* DHCP 최소 권장 버퍼 */
static uint8_t  s_dhcp_socket;
static bool     s_dhcp_active = false;

/* ── wiz_claw_net_init ──────────────────────────────────────── */

void wiz_claw_net_init(const wiz_claw_net_config_t *config)
{
    if (!config) { return; }

    s_dns_socket = config->dns_socket;
    s_dhcp_socket = config->dhcp_socket;
    memcpy(s_dns_server_ip, config->dns_server_ip, 4);

    DNS_init(s_dns_socket, s_dns_buf);
    WIZ_CLAW_LOG_I(TAG, "DNS init socket=%u server=%u.%u.%u.%u",
                   s_dns_socket,
                   s_dns_server_ip[0], s_dns_server_ip[1],
                   s_dns_server_ip[2], s_dns_server_ip[3]);
}

/* ── wiz_claw_net_dns_resolve ───────────────────────────────── */

wiz_claw_err_t wiz_claw_net_dns_resolve(const char *hostname, uint8_t out_ip[4])
{
    int8_t ret;

    if (!hostname || !out_ip) { return WIZ_CLAW_ERR_INVALID_ARG; }

    WIZ_CLAW_LOG_I(TAG, "DNS query: %s", hostname);

    /* DNS_run: 1=성공, 0=실패, -1=도메인명 너무 길다 */
    ret = DNS_run(s_dns_server_ip, (uint8_t *)hostname, out_ip);
    if (ret != 1) {
        WIZ_CLAW_LOG_E(TAG, "DNS_run failed ret=%d", ret);
        return WIZ_CLAW_ERR_NOT_FOUND;
    }

    WIZ_CLAW_LOG_I(TAG, "DNS ok: %s -> %u.%u.%u.%u",
                   hostname, out_ip[0], out_ip[1], out_ip[2], out_ip[3]);
    return WIZ_CLAW_OK;
}

/* ── wiz_claw_net_dhcp_run ──────────────────────────────────── */

/* DHCP 이벤트 콜백 (ioLibrary가 호출) */
static void dhcp_assign_cb(void)   { WIZ_CLAW_LOG_I(TAG, "DHCP assigned"); }
static void dhcp_update_cb(void)   { WIZ_CLAW_LOG_I(TAG, "DHCP updated"); }
static void dhcp_conflict_cb(void) { WIZ_CLAW_LOG_W(TAG, "DHCP conflict"); }

wiz_claw_err_t wiz_claw_net_dhcp_run(wiz_NetInfo *inout_net_info)
{
    int8_t  ret;
    uint32_t timeout_cnt = 0;

    if (!inout_net_info) { return WIZ_CLAW_ERR_INVALID_ARG; }

    DHCP_init(s_dhcp_socket, s_dhcp_buf);
    reg_dhcp_cbfunc(dhcp_assign_cb, dhcp_update_cb, dhcp_conflict_cb);
    s_dhcp_active = true;

    WIZ_CLAW_LOG_I(TAG, "DHCP start socket=%u ...", s_dhcp_socket);

    /* DHCP_run을 반복 호출 (ioLibrary 설계) */
    while (1) {
        ret = DHCP_run();

        /*
         * DHCP_IP_LEASED belongs here as much as the other two.
         *
         * ioLibrary returns DHCP_IP_ASSIGN only on the single pass that moves
         * into STATE_DHCP_LEASED, and DHCP_IP_LEASED on every pass afterwards.
         * The assign callback fires on that first pass while the return value is
         * still DHCP_RUNNING, so a loop watching only for ASSIGN misses the
         * moment and then waits out its full timeout - printing "DHCP assigned"
         * and "DHCP timeout" one after the other, which is what sent a board
         * with a perfectly good lease back to its fallback address.
         */
        if (ret == DHCP_IP_ASSIGN || ret == DHCP_IP_CHANGED ||
            ret == DHCP_IP_LEASED) {
            getIPfromDHCP(inout_net_info->ip);
            getGWfromDHCP(inout_net_info->gw);
            getSNfromDHCP(inout_net_info->sn);
            getDNSfromDHCP(inout_net_info->dns);
            inout_net_info->dhcp = NETINFO_DHCP;
            WIZ_CLAW_LOG_I(TAG, "DHCP ok: %u.%u.%u.%u",
                           inout_net_info->ip[0], inout_net_info->ip[1],
                           inout_net_info->ip[2], inout_net_info->ip[3]);
            return WIZ_CLAW_OK;
        }

        if (ret == DHCP_FAILED) {
            WIZ_CLAW_LOG_E(TAG, "DHCP failed");
            s_dhcp_active = false;
            return WIZ_CLAW_ERR_FAIL;
        }

        /* DHCP_RUNNING: 계속 대기 (최대 60초) */
        sleep_ms(100);
        timeout_cnt++;
        if (timeout_cnt > 600) {     /* 60초 */
            WIZ_CLAW_LOG_E(TAG, "DHCP timeout");
            s_dhcp_active = false;
            return WIZ_CLAW_ERR_FAIL;
        }
    }
}

/* ── wiz_claw_net_1s_tick ───────────────────────────────────── */

void wiz_claw_net_1s_tick(void)
{
    DNS_time_handler();
    if (s_dhcp_active) {
        DHCP_time_handler();
    }
}

/* ── wiz_claw_net_print_info ────────────────────────────────── */

void wiz_claw_net_print_info(const wiz_NetInfo *net_info)
{
    wiz_NetInfo tmp;

    if (!net_info) {
        ctlnetwork(CN_GET_NETINFO, &tmp);
        net_info = &tmp;
    }

    printf("--- Network Info ---\n");
    printf(" MAC  : %02X:%02X:%02X:%02X:%02X:%02X\n",
           net_info->mac[0], net_info->mac[1], net_info->mac[2],
           net_info->mac[3], net_info->mac[4], net_info->mac[5]);
    printf(" IP   : %u.%u.%u.%u\n",
           net_info->ip[0], net_info->ip[1], net_info->ip[2], net_info->ip[3]);
    printf(" SN   : %u.%u.%u.%u\n",
           net_info->sn[0], net_info->sn[1], net_info->sn[2], net_info->sn[3]);
    printf(" GW   : %u.%u.%u.%u\n",
           net_info->gw[0], net_info->gw[1], net_info->gw[2], net_info->gw[3]);
    printf(" DNS  : %u.%u.%u.%u\n",
           net_info->dns[0], net_info->dns[1], net_info->dns[2], net_info->dns[3]);
    printf("--------------------\n");
}
