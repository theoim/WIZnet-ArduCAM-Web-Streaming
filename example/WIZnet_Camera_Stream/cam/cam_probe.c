/**
 * @file    cam_probe.c
 * @brief   Which camera is this? Bus scan plus the ID registers of known parts.
 *
 * Known parts and where they keep their ID (datasheets / existing drivers):
 *
 *   addr  part                     ID registers                 expected
 *   0x1F  Arducam module controller 16-bit 0x0000,0x0001,0x0002 undocumented
 *         (Mega / Quick-Bootup DVP)  SENSOR_ID_H/L, FIRMWARE_VER  (this is the
 *                                    mega_ccm_regs.h map)         question)
 *   0x30  OV2640                   8-bit 0x0A,0x0B, bank 1       0x26 0x42
 *                                  (0xFF = 0x01 selects it)
 *   0x21  OV7640 / OV7670          8-bit 0x0A,0x0B               0x76 0x48 / 0x76 0x73
 *   0x3C  OV5640 / OV3660          16-bit 0x300A,0x300B          0x56 0x40 / 0x36 0x60
 *   0x24  HM01B0                   16-bit 0x0000,0x0001          0x01 0xB0
 *
 * For the Arducam controller the first sixteen 16-bit registers and the
 * 0x0100 sensor block are dumped as well: nothing about that map is public,
 * and the raw values are what lets the ID be interpreted afterwards.
 */

#include <stdio.h>
#include "cam_probe.h"
#include "cam_sccb.h"

static int probe_arducam_ctrl(uint8_t a)
{
    uint8_t idh = 0, idl = 0, fw = 0;
    int e = sccb_read16(a, 0x0000, &idh) | sccb_read16(a, 0x0001, &idl) |
            sccb_read16(a, 0x0002, &fw);
    printf("  0x%02X Arducam module controller: SENSOR_ID 0x%02X%02X  "
           "FIRMWARE_VER 0x%02X%s\n", a, idh, idl, fw, e ? "  (bus error)" : "");

    printf("       raw 0x0000..0x000F:");
    for (uint16_t r = 0x0000; r <= 0x000F; r++) {
        uint8_t v = 0;
        if (sccb_read16(a, r, &v)) printf(" --");
        else printf(" %02X", v);
    }
    printf("\n       raw 0x0100..0x010F:");
    for (uint16_t r = 0x0100; r <= 0x010F; r++) {
        uint8_t v = 0;
        if (sccb_read16(a, r, &v)) printf(" --");
        else printf(" %02X", v);
    }
    printf("\n       raw 0x0120..0x012F:");
    for (uint16_t r = 0x0120; r <= 0x012F; r++) {
        uint8_t v = 0;
        if (sccb_read16(a, r, &v)) printf(" --");
        else printf(" %02X", v);
    }
    printf("\n");
    return e ? 0 : 1;
}

static int probe_ov2640(uint8_t a)
{
    uint8_t h = 0, l = 0;
    int e = sccb_write8(a, 0xFF, 0x01) | sccb_read8(a, 0x0A, &h) | sccb_read8(a, 0x0B, &l);
    bool ok = !e && h == 0x26 && l == 0x42;
    printf("  0x%02X OV2640? PID 0x%02X VER 0x%02X -> %s\n", a, h, l,
           ok ? "OV2640" : (e ? "bus error" : "not an OV2640"));
    return ok;
}

static int probe_ov76x0(uint8_t a)
{
    uint8_t h = 0, l = 0;
    int e = sccb_read8(a, 0x0A, &h) | sccb_read8(a, 0x0B, &l);
    const char *n = e ? "bus error" :
                    (h == 0x76 && l == 0x48) ? "OV7640" :
                    (h == 0x76 && l == 0x73) ? "OV7670" : "unknown 0x21 part";
    printf("  0x%02X OV76x0? PID 0x%02X VER 0x%02X -> %s\n", a, h, l, n);
    return !e && h == 0x76;
}

static int probe_ov5640_3660(uint8_t a)
{
    uint8_t h = 0, l = 0;
    int e = sccb_read16(a, 0x300A, &h) | sccb_read16(a, 0x300B, &l);
    const char *n = e ? "bus error" :
                    (h == 0x56 && l == 0x40) ? "OV5640" :
                    (h == 0x36 && l == 0x60) ? "OV3660" : "unknown 0x3C part";
    printf("  0x%02X OV5640/OV3660? CHIP_ID 0x%02X%02X -> %s\n", a, h, l, n);
    return !e && ((h == 0x56 && l == 0x40) || (h == 0x36 && l == 0x60));
}

static int probe_hm01b0(uint8_t a)
{
    uint8_t h = 0, l = 0;
    int e = sccb_read16(a, 0x0000, &h) | sccb_read16(a, 0x0001, &l);
    bool ok = !e && h == 0x01 && l == 0xB0;
    printf("  0x%02X HM01B0? MODEL_ID 0x%02X%02X -> %s\n", a, h, l,
           ok ? "HM01B0" : (e ? "bus error" : "not an HM01B0"));
    return ok;
}

int cam_probe_report(void)
{
    printf("SCCB scan (0x08..0x77):");
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (sccb_present(a)) {
            printf(" 0x%02X", a);
            found++;
        }
    }
    printf("%s\n", found ? "" : " nothing answered");

    int known = 0;
    if (sccb_present(0x1F)) known += probe_arducam_ctrl(0x1F);
    if (sccb_present(0x30)) known += probe_ov2640(0x30);
    if (sccb_present(0x21)) known += probe_ov76x0(0x21);
    if (sccb_present(0x3C)) known += probe_ov5640_3660(0x3C);
    if (sccb_present(0x24)) known += probe_hm01b0(0x24);

    if (!found) {
        printf("  No device ACKs. Check 3V3, PWDN low, and - for a module without\n"
               "  an oscillator - XCLK, before suspecting SDA/SCL.\n");
    } else if (!known) {
        printf("  Devices answer, none of them a known camera address.\n");
    }
    return known;
}
