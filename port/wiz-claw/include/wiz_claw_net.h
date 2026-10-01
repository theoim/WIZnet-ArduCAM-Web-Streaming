/**
 * wiz_claw_net.h — 네트워크 초기화 / DNS / DHCP 헬퍼
 *
 * ioLibrary_Driver의 DNS, DHCP 모듈을 wiz-claw 에러 타입으로 래핑.
 * wizchip_initialize() + network_initialize() 이후에 호출.
 *
 * 의존: ioLibrary_Driver (dns.h, dhcp.h, wizchip_conf.h)
 *       Pico SDK (pico/stdlib.h - sleep_ms)
 */
#pragma once

#include "wiz_claw.h"
#include "wizchip_conf.h"  /* wiz_NetInfo */

/* ── 소켓 번호 기본값 (8개 중 상위 2개를 프로토콜용으로 예약) ─ */
#ifndef WIZ_CLAW_SOCK_DNS
#define WIZ_CLAW_SOCK_DNS   5   /* UDP - DNS 쿼리 */
#endif
#ifndef WIZ_CLAW_SOCK_DHCP
#define WIZ_CLAW_SOCK_DHCP  6   /* UDP - DHCP */
#endif

/* ── 네트워크 설정 ────────────────────────────────────────────── */
typedef struct {
    uint8_t dns_server_ip[4];   /* DNS 서버 IP (예: {8,8,8,8}) */
    uint8_t dns_socket;         /* DNS용 소켓 번호 */
    uint8_t dhcp_socket;        /* DHCP용 소켓 번호 */
} wiz_claw_net_config_t;

/* ── API ──────────────────────────────────────────────────────── */

/**
 * DNS / DHCP 모듈 초기화.
 * wizchip_initialize() 이후, 첫 DNS/DHCP 호출 전에 한 번 실행.
 */
void wiz_claw_net_init(const wiz_claw_net_config_t *config);

/**
 * 호스트네임 → IP 변환 (블로킹, 최대 DNS_WAIT_TIME * MAX_DNS_RETRY 초).
 *
 * @param hostname  도메인 문자열 (예: "api.openai.com")
 * @param out_ip    결과 IP를 채울 4바이트 배열
 */
wiz_claw_err_t wiz_claw_net_dns_resolve(const char *hostname, uint8_t out_ip[4]);

/**
 * DHCP로 IP 자동 획득 (블로킹, 최대 ~60초).
 * 성공 시 out_net_info 필드들(ip, sn, gw, dns)이 채워짐.
 */
wiz_claw_err_t wiz_claw_net_dhcp_run(wiz_NetInfo *inout_net_info);

/**
 * DNS / DHCP 1초 타이머 핸들러.
 * 반드시 1초 주기 타이머 인터럽트 또는 루프에서 호출해야 함.
 */
void wiz_claw_net_1s_tick(void);

/**
 * 현재 네트워크 정보를 시리얼로 출력 (디버그용).
 */
void wiz_claw_net_print_info(const wiz_NetInfo *net_info);
