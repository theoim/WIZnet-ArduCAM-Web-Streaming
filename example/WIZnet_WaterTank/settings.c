/**
 * @file    settings.c
 * @brief   See settings.h.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#ifndef USE_CORE1
#define USE_CORE1 0
#endif

#if USE_CORE1
#include "pico/multicore.h"
#endif

#include "settings.h"
#include "config.h"

#define SETTINGS_MAGIC    0x57544B31u   /* "WTK1" */
/*
 * Bumped to 2 when the camera resolution joined the struct. A stored copy from
 * version 1 is rejected rather than migrated, so a board that is updated comes
 * up on the config.h defaults and has to be addressed once more. For a setting
 * someone enters from a phone that is cheaper than a migration path nobody will
 * exercise again.
 */
/* 4: the two detector thresholds joined the struct. */
#define SETTINGS_VERSION  4

/*
 * The last sector of flash.
 *
 * Last rather than a fixed address so that growing the firmware never walks
 * into the settings: the image starts at the bottom and this stays pinned to
 * the top. A build that ever got large enough to reach it would fail to link
 * long before it overwrote anything.
 */
#define SETTINGS_OFFSET   (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

settings_t g_set;

/* ------------------------------------------------------------------- crc32 */

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        }
    }
    return ~c;
}

static uint32_t settings_crc(const settings_t *s)
{
    return crc32(s, offsetof(settings_t, crc));
}

/* ---------------------------------------------------------------- build id */

/*
 * The end of the firmware image, from the linker. Everything from the start of
 * flash to here is the program; the settings sector sits far above it.
 */
extern char __flash_binary_end;

uint32_t settings_build_id(void)
{
    static uint32_t cached;

    if (cached == 0) {
        const uint8_t *start = (const uint8_t *)XIP_BASE;
        size_t len = (size_t)(&__flash_binary_end - (char *)XIP_BASE);

        /*
         * About 30 ms for a 600 KB image, once, before anything else runs. The
         * alternative - hashing only the first few KB - would miss a change
         * anywhere else in the program, which is most of it.
         */
        cached = crc32(start, len);
        if (cached == 0) cached = 1;        /* 0 means "not computed yet" */
    }
    return cached;
}

/* ----------------------------------------------------------------- parsing */

bool settings_parse_ip(const char *str, uint8_t out[4])
{
    unsigned a, b, c, d;

    if (!str) return false;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;

    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

/* -------------------------------------------------------------- resolution */

bool settings_res_valid(uint8_t res)
{
    return res == SETTINGS_RES_QVGA ||
           res == SETTINGS_RES_VGA  ||
           res == SETTINGS_RES_HD;
}

const char *settings_res_name(uint8_t res)
{
    switch (res) {
    case SETTINGS_RES_QVGA: return "QVGA 320x240";
    case SETTINGS_RES_VGA:  return "VGA 640x480";
    case SETTINGS_RES_HD:   return "HD 1280x720";
    default:                return "unknown";
    }
}

/* ---------------------------------------------------------------- defaults */

void settings_defaults(settings_t *s)
{
    static const uint8_t ip[]  = NET_IP;
    static const uint8_t sn[]  = NET_SUBNET;
    static const uint8_t gw[]  = NET_GATEWAY;
    static const uint8_t dns[] = NET_DNS;

    memset(s, 0, sizeof(*s));

    s->magic   = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;
    s->size    = (uint16_t)sizeof(*s);

    memcpy(s->ip, ip, 4);
    memcpy(s->sn, sn, 4);
    memcpy(s->gw, gw, 4);
    memcpy(s->dns, dns, 4);
    s->http_port = HTTP_PORT;
    s->res       = CAM_RES_DEFAULT;
    s->use_dhcp  = NET_USE_DHCP;
    s->loud_k      = DETECT_LOUD_K;
    s->enter_ratio = DETECT_ENTER_RATIO;
    s->build_id  = settings_build_id();

    /*
     * PUBLIC_URL_BASE is one string in config.h and two fields here, because
     * the page edits them separately. Splitting it at build time keeps the
     * header readable for whoever sets the defaults.
     */
    {
        const char *u = PUBLIC_URL_BASE;
        const char *h = strstr(u, "://");
        h = h ? h + 3 : u;

        const char *colon = strchr(h, ':');
        size_t hl = colon ? (size_t)(colon - h) : strlen(h);
        if (hl >= SETTINGS_HOST_MAX) hl = SETTINGS_HOST_MAX - 1;

        memcpy(s->pub_host, h, hl);
        s->pub_host[hl] = '\0';
        s->pub_port = colon ? (uint16_t)atoi(colon + 1) : 80;
    }

    strncpy(s->webhook, WEBHOOK_URL, SETTINGS_WEBHOOK_MAX - 1);

    s->crc = settings_crc(s);
}

/* ------------------------------------------------------------------- flash */

bool settings_load(void)
{
    const settings_t *f =
        (const settings_t *)(XIP_BASE + SETTINGS_OFFSET);

    if (f->magic   == SETTINGS_MAGIC &&
        f->version == SETTINGS_VERSION &&
        f->size    == sizeof(settings_t) &&
        f->crc     == settings_crc(f)) {

        if (f->build_id == settings_build_id()) {
            memcpy(&g_set, f, sizeof(g_set));
            printf("[set] loaded from flash\n");
            return true;
        }

        /*
         * Same board, different firmware. The stored copy is intact and is
         * thrown away anyway: a fresh image is meant to come up on its own
         * defaults - DHCP, HD, the webhook in config.h - and not on whatever the
         * previous build was left configured with.
         *
         * A reset does not reach here, because a reset runs the same image and
         * the ids match. That is the whole distinction.
         */
        printf("[set] firmware changed - discarding stored settings\n");
    } else {
        printf("[set] nothing valid stored\n");
    }

    settings_defaults(&g_set);
    printf("[set] using built-in defaults (DHCP, %s)\n",
           settings_res_name(g_set.res));
    return false;
}

/*
 * Flash is programmed in whole 256-byte pages, so the staging buffer is the
 * struct rounded up to a page boundary - 292 bytes becomes two pages.
 *
 * It used to be one page with `if (sizeof(s) > FLASH_PAGE_SIZE) return false;`
 * guarding it, which is how a save that had never once worked reported itself:
 * the struct passed 256 bytes the moment the webhook field was given room for a
 * real Discord URL, and from then on every SAVE answered "플래시 쓰기에
 * 실패했습니다" without ever touching flash. The guard was right that the write
 * would be malformed and wrong to treat a build-time fact as a runtime error -
 * so it is a build-time check now, and the buffer follows the struct.
 */
#define SETTINGS_PAGES   (((sizeof(settings_t) + FLASH_PAGE_SIZE - 1) / \
                            FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE)

_Static_assert(SETTINGS_PAGES <= FLASH_SECTOR_SIZE,
               "settings no longer fit the sector reserved for them");

bool settings_save(const settings_t *in)
{
    static uint8_t page[SETTINGS_PAGES];
    settings_t s = *in;

    s.magic    = SETTINGS_MAGIC;
    s.version  = SETTINGS_VERSION;
    s.size     = (uint16_t)sizeof(s);
    s.build_id = settings_build_id();
    s.crc      = settings_crc(&s);

    memset(page, 0xFF, sizeof(page));
    memcpy(page, &s, sizeof(s));

    /*
     * Interrupts off for the whole operation.
     *
     * Erasing and programming stop the processor fetching from flash, and every
     * interrupt handler in this firmware lives there. One DMA completion
     * arriving mid-erase would jump into flash that is not readable - which is
     * not a crash that leaves a useful message behind.
     *
     * The cost is real and visible: a sector erase runs for tens of
     * milliseconds, so the microphone loses buffers and the overrun counter
     * moves. That is the correct trade for an operation someone deliberately
     * asked for, and it is why saving is a button rather than something that
     * happens on its own.
     */
#if USE_CORE1
    /*
     * The other core has to be parked, not merely interrupted.
     *
     * Erasing takes the whole XIP window away, and core 0 is executing from it.
     * Disabling interrupts says nothing to a second core that is in the middle
     * of fetching an instruction - it would fetch from flash that cannot answer,
     * and the board would stop with nothing on the console to say why. The
     * lockout holds core 0 in a known place inside RAM until the write is done.
     *
     * It costs core 0 the few tens of milliseconds the erase takes, which the
     * overrun counter will report as lost audio. That is the correct trade for
     * something a person deliberately pressed.
     */
    multicore_lockout_start_blocking();
#endif

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_OFFSET, page, SETTINGS_PAGES);
    restore_interrupts(ints);

#if USE_CORE1
    multicore_lockout_end_blocking();
#endif

    const settings_t *f = (const settings_t *)(XIP_BASE + SETTINGS_OFFSET);
    if (memcmp(f, &s, sizeof(s)) != 0) {
        printf("[set] verify FAILED after write\n");
        return false;
    }

    memcpy(&g_set, &s, sizeof(g_set));
    printf("[set] saved\n");
    return true;
}
