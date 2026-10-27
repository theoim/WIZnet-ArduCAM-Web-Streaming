/**
 * @file    settings_store.h
 * @brief   Settings kept in two flash sectors, written alternately.
 *
 * One record per sector. A save always goes to the sector that does NOT hold
 * the current record, so a power cut at any point leaves the previous record
 * untouched and the board boots with it:
 *
 *   1. erase the other sector
 *   2. program header + payload, read back and compare
 *   3. program the commit word in the sector's last page, read back
 *
 * A record counts only when its header CRC, payload CRC and commit word all
 * check. Of two valid records the higher generation wins (wrap-safe).
 *
 * The store knows nothing about what the payload means: the caller passes a
 * blob and a schema version. Flash access goes through three callbacks, so the
 * same code runs against the real flash and against the host test's RAM model.
 */

#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SSTORE_SECTOR   4096u
#define SSTORE_PAGE     256u
#define SSTORE_HDR_SIZE 24u
/* Header and payload share the pages before the last one; the last page holds
 * only the commit word. */
#define SSTORE_PAYLOAD_MAX (SSTORE_SECTOR - SSTORE_PAGE - SSTORE_HDR_SIZE)

/**
 * Offsets are relative to the start of the two-sector region: slot 0 at 0,
 * slot 1 at SSTORE_SECTOR. erase() takes a sector offset; program() a page
 * offset and a whole number of pages. All return 0 on success.
 */
typedef struct {
    int (*read)(uint32_t off, void *buf, uint32_t len);
    int (*erase)(uint32_t off);
    int (*program)(uint32_t off, const void *buf, uint32_t len);
} sstore_flash_t;

typedef enum {
    SSTORE_OK = 0,
    SSTORE_E_NONE,      /* no valid record in either slot                  */
    SSTORE_E_TOO_BIG,   /* payload larger than SSTORE_PAYLOAD_MAX or cap   */
    SSTORE_E_FLASH,     /* a flash callback failed                          */
    SSTORE_E_VERIFY,    /* read back differs from what was programmed       */
} sstore_err_t;

typedef struct {
    const sstore_flash_t *f;
    int      cur;       /* slot holding the record loaded/saved last, or -1 */
    uint32_t gen;       /* its generation (0 when cur is -1)               */
    bool     scanned;   /* slots examined; save() scans first otherwise    */
} sstore_t;

const char *sstore_err_name(sstore_err_t e);

void sstore_init(sstore_t *s, const sstore_flash_t *f);

/**
 * Find the newest valid record and copy its payload into @p buf. On success
 * *len and *version describe it. SSTORE_E_TOO_BIG when it does not fit @p cap
 * (nothing copied, but the slot is still remembered for the next save).
 */
sstore_err_t sstore_load(sstore_t *s, void *buf, uint32_t cap, uint32_t *len,
                         uint16_t *version);

/** Write a new record into the other slot. Leaves the current record alone. */
sstore_err_t sstore_save(sstore_t *s, const void *payload, uint32_t len,
                         uint16_t version);

/**
 * Erase both slots: the next load finds nothing (factory reset). The slot NOT
 * holding the current record goes first, so a power cut part way leaves
 * either the current record or nothing - never an older record coming back
 * (an old password, say).
 */
sstore_err_t sstore_erase_all(sstore_t *s);

uint32_t sstore_crc32(const void *data, uint32_t len);

#endif /* SETTINGS_STORE_H */
