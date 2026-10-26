/**
 * @file    settings_flash.c
 * @brief   settings_store callbacks on the RP2350's QSPI flash.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "settings_flash.h"

extern char __flash_binary_end;

static uint32_t s_chip_bytes;
static uint32_t s_region;          /* flash offset of slot 0 */

uint32_t settings_flash_chip_bytes(void) { return s_chip_bytes; }
uint32_t settings_flash_region_off(void) { return s_region; }

bool settings_flash_init(void)
{
    uint8_t tx[4] = { 0x9F, 0, 0, 0 };     /* JEDEC Read ID */
    uint8_t rx[4] = { 0 };

    /* flash_do_cmd() leaves XIP for the duration: nothing may run from flash,
     * which before core 1 is launched means only this core's interrupts. */
    uint32_t irq = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, sizeof(tx));
    restore_interrupts(irq);

    /* Capacity byte is log2(bytes) for the usual SPI NOR parts. */
    if (rx[3] < 0x11 || rx[3] > 0x19) {        /* 128 KB .. 32 MB */
        printf("[settings] JEDEC %02X %02X %02X: size not recognised - store disabled\n",
               rx[1], rx[2], rx[3]);
        return false;
    }
    uint32_t chip   = 1u << rx[3];
    uint32_t region = chip - 2u * SSTORE_SECTOR;
    uint32_t image  = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
    if (image > region) {
        printf("[settings] firmware ends at 0x%06lX, past the store at 0x%06lX - store disabled\n",
               (unsigned long)image, (unsigned long)region);
        return false;
    }
    s_chip_bytes = chip;
    s_region     = region;
    printf("[settings] flash %lu KB (JEDEC %02X %02X %02X), store at 0x%06lX, firmware ends 0x%06lX\n",
           (unsigned long)(chip / 1024), rx[1], rx[2], rx[3], (unsigned long)region,
           (unsigned long)image);
    return true;
}

static bool in_region(uint32_t off, uint32_t len)
{
    return s_chip_bytes && off <= 2u * SSTORE_SECTOR && len <= 2u * SSTORE_SECTOR - off;
}

/* Reads go through XIP; flash_range_program/erase flush the cache after. */
static int f_read(uint32_t off, void *buf, uint32_t len)
{
    if (!in_region(off, len)) return -1;
    memcpy(buf, (const void *)(XIP_BASE + s_region + off), len);
    return 0;
}

typedef struct {
    uint32_t       off;
    const uint8_t *data;
    uint32_t       len;
} flash_job_t;

static void do_erase(void *p)
{
    const flash_job_t *j = p;
    flash_range_erase(j->off, SSTORE_SECTOR);
}

static void do_program(void *p)
{
    const flash_job_t *j = p;
    flash_range_program(j->off, j->data, j->len);
}

/* flash_safe_execute() parks core 1 in RAM (multicore lockout) and disables
 * interrupts here for the duration. 1 s covers core 1 noticing the lockout. */
static int f_erase(uint32_t off)
{
    if (!in_region(off, SSTORE_SECTOR) || off % SSTORE_SECTOR) return -1;
    flash_job_t j = { s_region + off, NULL, 0 };
    int rc = flash_safe_execute(do_erase, &j, 1000);
    if (rc != PICO_OK) printf("[settings] erase 0x%06lX: rc %d\n", (unsigned long)j.off, rc);
    return rc == PICO_OK ? 0 : -1;
}

static int f_program(uint32_t off, const void *buf, uint32_t len)
{
    if (!in_region(off, len) || off % SSTORE_PAGE || len % SSTORE_PAGE) return -1;
    flash_job_t j = { s_region + off, buf, len };
    int rc = flash_safe_execute(do_program, &j, 1000);
    if (rc != PICO_OK) printf("[settings] program 0x%06lX: rc %d\n", (unsigned long)j.off, rc);
    return rc == PICO_OK ? 0 : -1;
}

static const sstore_flash_t k_ops = { f_read, f_erase, f_program };

const sstore_flash_t *settings_flash_ops(void)
{
    return &k_ops;
}
