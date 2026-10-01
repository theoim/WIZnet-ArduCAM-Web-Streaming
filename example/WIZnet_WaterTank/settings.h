/**
 * @file    settings.h
 * @brief   The values a commissioning engineer sets, kept across power cuts.
 *
 * Everything here used to live in config.h, which meant that changing a basement
 * router's address needed a toolchain, a USB cable and someone who knows how to
 * build the firmware. For a box that gets installed once and adjusted by whoever
 * is standing in front of it, that is the wrong place for them.
 *
 * They go in the last sector of flash. Defaults from config.h are used when
 * nothing valid has been written yet, so a brand-new board still comes up on a
 * known address and can be reached to be configured.
 */

#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_HOST_MAX     64
#define SETTINGS_WEBHOOK_MAX  192

/*
 * The two camera modes the panel offers, as the driver's own res_t values.
 *
 * Named here rather than including the camera header, because this file is
 * about what is stored and the stored byte has to keep its meaning across a
 * driver that may gain or reorder modes. FHD is deliberately absent: the tank
 * only needs to show a water level, and a 1080p frame costs capture time and
 * wire time that buy nothing at that job.
 */
#define SETTINGS_RES_QVGA     1   /* 320x240  */
#define SETTINGS_RES_VGA      2   /* 640x480  */
#define SETTINGS_RES_HD       3   /* 1280x720 */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;

    uint8_t  ip[4];
    uint8_t  sn[4];
    uint8_t  gw[4];
    uint8_t  dns[4];
    uint16_t http_port;

    /*
     * How the outside world reaches this box. The board cannot work this out
     * for itself - it only ever sees its private address - so it has to be
     * told, and it goes into the alert to make the link tappable.
     */
    char     pub_host[SETTINGS_HOST_MAX];
    uint16_t pub_port;

    char     webhook[SETTINGS_WEBHOOK_MAX];

    /*
     * One of SETTINGS_RES_QVGA / SETTINGS_RES_HD. Applied once at boot, which
     * is why a save restarts the board: changing the frame size re-clocks the
     * sensor and resets its control registers, and doing that underneath a
     * stream that is mid-frame is a worse path than a reboot.
     */
    uint8_t  res;

    /*
     * Non-zero to ask the router for an address at boot, zero to use the one
     * above.
     *
     * Taken out of the reserved bytes rather than appended, so the struct keeps
     * its size and its version: a copy stored before this field existed has
     * zeroes there, which reads as "static" - exactly what those boards were
     * doing. Nobody loses their settings to this.
     */
    uint8_t  use_dhcp;
    uint8_t  rsvd[2];      /* explicit, so no implicit padding enters the crc */

    /*
     * Which firmware wrote this. A checksum of the image itself, so it changes
     * whenever the binary does.
     *
     * Stored settings are kept across a reset and discarded across a firmware
     * upload, and this field is what tells the two apart. A board being
     * developed on gets new code every few minutes, and settings written by the
     * build before last are a quiet trap: the new default is HD and DHCP, the
     * stored copy says QVGA and a fixed address, and the stored copy wins
     * silently. Someone then spends an afternoon on a default that was never in
     * effect.
     *
     * Deliberately not __DATE__/__TIME__: those live in whichever object file
     * names them, and that file is not recompiled when a different one changes -
     * so two different firmwares would claim the same identity.
     */
    uint32_t build_id;

    uint32_t crc;           /* over everything above */
} settings_t;

/** The live copy. Read freely; change only through settings_save(). */
extern settings_t g_set;

/**
 * Load from flash into g_set, falling back to the config.h defaults when the
 * stored copy is missing or corrupt.
 *
 * @return true if a valid stored copy was found.
 */
bool settings_load(void);

/**
 * Write @p s to flash and copy it into g_set.
 *
 * Erasing a sector stops the processor fetching instructions from flash for
 * tens of milliseconds, so interrupts are off for the duration. The microphone
 * loses a few buffers and reports overruns; that is the price of a save and it
 * only happens when somebody presses the button.
 *
 * @return true on success.
 */
bool settings_save(const settings_t *s);

/** A checksum of the running firmware image. See settings_t::build_id. */
uint32_t settings_build_id(void);

/** Fill @p s with the compile-time defaults. */
void settings_defaults(settings_t *s);

/** True if @p res is one of the three modes the panel offers. */
bool settings_res_valid(uint8_t res);

/** "HD 1280x720" and so on, for logs and for the panel. */
const char *settings_res_name(uint8_t res);

/** "a.b.c.d" into four bytes. Returns false if it is not an address. */
bool settings_parse_ip(const char *str, uint8_t out[4]);

#endif /* __SETTINGS_H__ */
