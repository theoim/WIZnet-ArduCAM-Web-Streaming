/**
 * @file    settings_store.c
 * @brief   Two-slot settings record - see settings_store.h.
 */

#include <string.h>

#include "settings_store.h"

#define SSTORE_MAGIC   0x54455343u   /* "CSET" little-endian */
#define SSTORE_FMT     1u
#define SSTORE_COMMIT  0x4D4D4F43u   /* "COMM"; stored XOR the generation  */
#define COMMIT_OFF     (SSTORE_SECTOR - SSTORE_PAGE)

const char *sstore_err_name(sstore_err_t e)
{
    switch (e) {
    case SSTORE_OK:       return "ok";
    case SSTORE_E_NONE:   return "no valid record";
    case SSTORE_E_TOO_BIG: return "too big";
    case SSTORE_E_FLASH:  return "flash error";
    case SSTORE_E_VERIFY: return "verify failed";
    }
    return "?";
}

uint32_t sstore_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t get16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

typedef struct {
    uint16_t version;
    uint32_t gen, len, pcrc;
} rec_t;

/* Validate the record in @p slot. Does not copy the payload. */
static bool slot_valid(const sstore_t *s, int slot, rec_t *r)
{
    uint32_t base = (uint32_t)slot * SSTORE_SECTOR;
    uint8_t  h[SSTORE_HDR_SIZE];
    uint8_t  chunk[64];

    if (s->f->read(base, h, sizeof(h)) != 0) return false;
    if (get32(h) != SSTORE_MAGIC || get16(h + 4) != SSTORE_FMT) return false;
    if (get32(h + 20) != sstore_crc32(h, 20)) return false;

    r->version = get16(h + 6);
    r->gen     = get32(h + 8);
    r->len     = get32(h + 12);
    r->pcrc    = get32(h + 16);
    if (r->len > SSTORE_PAYLOAD_MAX) return false;

    /* payload CRC, read in pieces so no sector-sized buffer is needed */
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t done = 0; done < r->len; ) {
        uint32_t n = r->len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (s->f->read(base + SSTORE_HDR_SIZE + done, chunk, n) != 0) return false;
        for (uint32_t i = 0; i < n; i++) {
            c ^= chunk[i];
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        done += n;
    }
    if (~c != r->pcrc) return false;

    uint8_t cw[4];
    if (s->f->read(base + COMMIT_OFF, cw, 4) != 0) return false;
    return get32(cw) == (SSTORE_COMMIT ^ r->gen);
}

void sstore_init(sstore_t *s, const sstore_flash_t *f)
{
    s->f       = f;
    s->cur     = -1;
    s->gen     = 0;
    s->scanned = false;
}

/* Pick the newest valid slot into s->cur / s->gen; -1 when there is none. */
static int scan(sstore_t *s, rec_t r[2])
{
    bool ok[2] = { slot_valid(s, 0, &r[0]), slot_valid(s, 1, &r[1]) };
    int  best  = -1;

    if (ok[0] && ok[1]) {
        best = ((int32_t)(r[1].gen - r[0].gen) > 0) ? 1 : 0;
    } else if (ok[0] || ok[1]) {
        best = ok[0] ? 0 : 1;
    }
    s->cur     = best;
    s->gen     = best >= 0 ? r[best].gen : 0;
    s->scanned = true;
    return best;
}

sstore_err_t sstore_load(sstore_t *s, void *buf, uint32_t cap, uint32_t *len,
                         uint16_t *version)
{
    rec_t r[2];
    int   best = scan(s, r);

    if (best < 0) {
        return SSTORE_E_NONE;
    }
    if (r[best].len > cap) {
        return SSTORE_E_TOO_BIG;
    }
    if (s->f->read((uint32_t)best * SSTORE_SECTOR + SSTORE_HDR_SIZE, buf, r[best].len) != 0) {
        return SSTORE_E_FLASH;
    }
    /* The payload was CRC-checked a moment ago; check what was copied too. */
    if (sstore_crc32(buf, r[best].len) != r[best].pcrc) {
        return SSTORE_E_VERIFY;
    }
    if (len) *len = r[best].len;
    if (version) *version = r[best].version;
    return SSTORE_OK;
}

/* Byte @p i of the image header|payload|0xFF. */
static uint8_t image_byte(const uint8_t *hdr, const uint8_t *payload, uint32_t len, uint32_t i)
{
    if (i < SSTORE_HDR_SIZE) return hdr[i];
    i -= SSTORE_HDR_SIZE;
    return i < len ? payload[i] : 0xFF;
}

static sstore_err_t program_verified(const sstore_t *s, uint32_t off, const uint8_t *page)
{
    uint8_t back[SSTORE_PAGE];
    if (s->f->program(off, page, SSTORE_PAGE) != 0) return SSTORE_E_FLASH;
    if (s->f->read(off, back, SSTORE_PAGE) != 0) return SSTORE_E_FLASH;
    return memcmp(back, page, SSTORE_PAGE) == 0 ? SSTORE_OK : SSTORE_E_VERIFY;
}

sstore_err_t sstore_save(sstore_t *s, const void *payload, uint32_t len, uint16_t version)
{
    if (len > SSTORE_PAYLOAD_MAX) {
        return SSTORE_E_TOO_BIG;
    }
    if (!s->scanned) {
        rec_t r[2];
        scan(s, r);                 /* never erase the record we have not seen */
    }
    int      slot = (s->cur == 0) ? 1 : 0;
    uint32_t gen  = s->gen + 1;
    uint32_t base = (uint32_t)slot * SSTORE_SECTOR;
    uint8_t  hdr[SSTORE_HDR_SIZE];
    uint8_t  page[SSTORE_PAGE];
    sstore_err_t e;

    if (gen == 0) gen = 1;          /* 0 means "nothing loaded" */
    put32(hdr, SSTORE_MAGIC);
    hdr[4] = (uint8_t)SSTORE_FMT; hdr[5] = (uint8_t)(SSTORE_FMT >> 8);
    hdr[6] = (uint8_t)version;    hdr[7] = (uint8_t)(version >> 8);
    put32(hdr + 8, gen);
    put32(hdr + 12, len);
    put32(hdr + 16, sstore_crc32(payload, len));
    put32(hdr + 20, sstore_crc32(hdr, 20));

    if (s->f->erase(base) != 0) {
        return SSTORE_E_FLASH;
    }
    uint32_t total = SSTORE_HDR_SIZE + len;
    for (uint32_t p = 0; p * SSTORE_PAGE < total; p++) {
        for (uint32_t i = 0; i < SSTORE_PAGE; i++) {
            page[i] = image_byte(hdr, payload, len, p * SSTORE_PAGE + i);
        }
        e = program_verified(s, base + p * SSTORE_PAGE, page);
        if (e != SSTORE_OK) return e;
    }

    memset(page, 0xFF, sizeof(page));
    put32(page, SSTORE_COMMIT ^ gen);
    e = program_verified(s, base + COMMIT_OFF, page);
    if (e != SSTORE_OK) return e;

    s->cur = slot;
    s->gen = gen;
    return SSTORE_OK;
}

sstore_err_t sstore_erase_all(sstore_t *s)
{
    if (!s->scanned) {
        rec_t r[2];
        scan(s, r);                 /* to know which slot is current */
    }
    int first = (s->cur == 0) ? 1 : 0;
    if (s->f->erase((uint32_t)first * SSTORE_SECTOR) != 0 ||
        s->f->erase((uint32_t)(1 - first) * SSTORE_SECTOR) != 0) {
        return SSTORE_E_FLASH;
    }
    s->cur = -1;
    s->gen = 0;
    s->scanned = true;
    return SSTORE_OK;
}
