/**
 * @file    cam_controls.c
 * @brief   The sensor control table and the three things generated from it.
 */

#include <stdio.h>
#include <string.h>

#include "cam_controls.h"
#include "exhibition_config.h"
#include "arducam_mega.h"
#include "mega_ccm_regs.h"

/*
 * Declared here rather than included: arducam_reg_write() is defined in
 * arducam_mega.c but its prototype is commented out in arducam_mega.h. The
 * exhibition example does not modify the driver it shares with the other
 * examples, so the declaration lives on this side.
 */
extern uint8_t arducam_reg_write(sensor_info_t *config, uint16_t reg, uint8_t value);

/* ---------------------------------- Table ---------------------------------- */
/*
 * RANGES ARE PROVISIONAL AND MUST BE CHECKED ON HARDWARE.
 *
 * They follow the ArduCAM MEGA level enumerations, where a control is a level
 * index rather than a signed amount - brightness 0 is "default" and 1..8 step
 * away from it in alternating directions, which is why the ranges start at 0
 * and are not symmetric. If a value turns out to do nothing on a given module,
 * correct the row: the panel is generated from this table, so the browser
 * follows without any other edit.
 *
 * Deliberately NOT here yet, and the reason matters:
 *
 *   AGC_MODE_REG (0x0130), MANUAL_AGC_REG, MANUAL_EXP_H/L_REG
 *
 * Their encoding is a mode select rather than a plain value, and this file
 * cannot confirm the bit layout from anything in this repository. Writing a
 * guessed value into a sensor mode register is how a camera ends up wedged
 * halfway through an exhibition, so they stay out until they can be checked
 * against a module. Everything below writes one register with one value.
 */
static const cam_ctrl_t k_ctrls[] = {
    /* name          label            group          register            min max def */
    { "brightness",  "Brightness",    "Image",       BRIGHTNESS_REG,      0,  8,  0 },
    { "contrast",    "Contrast",      "Image",       CONTRAST_REG,        0,  6,  0 },
    { "saturation",  "Saturation",    "Image",       SATURATION_REG,      0,  6,  0 },
    { "sharpness",   "Sharpness",     "Image",       SHARPNESS_REG,       0,  8,  0 },
    { "quality",     "JPEG quality",  "Image",       IMAGE_QUALITY_REG,   0,  2,  0 },

    { "ev",          "EV compensate", "Exposure",    EXP_COMPENSATE_REG,  0,  6,  0 },
    { "awb",         "White balance", "Exposure",    AWB_MODE_REG,        0,  4,  0 },

    { "effect",      "Colour effect", "Effect",      SPECIAL_REG,         0,  8,  0 },

    { "focus",       "Auto focus",    "Lens",        FOCUS_REG,           0,  1,  0 },
    { "flip",        "Flip",          "Lens",        IMAGE_FLIP_REG,      0,  1,  0 },
    { "mirror",      "Mirror",        "Lens",        IMAGE_MIRROR_REG,    0,  1,  0 },
};

#define CTRL_COUNT ((int)(sizeof(k_ctrls) / sizeof(k_ctrls[0])))

/*
 * How many rows are live. Zero unless EXHIBITION_SENSOR_CONTROLS is set - see
 * the note there. The table stays compiled in either way, so turning it on is
 * a one-line change rather than a re-import.
 */
#if EXHIBITION_SENSOR_CONTROLS
#define ACTIVE_COUNT CTRL_COUNT
#else
#define ACTIVE_COUNT 0
#endif

static int16_t g_value[CTRL_COUNT];
/*
 * Has this control ever been set from the page?
 *
 * Nothing in the table is written to the sensor until someone asks for it.
 * The ranges above are provisional, and pushing eleven unverified values into
 * a sensor at boot is the same mistake the AGC registers were left out to
 * avoid - it just takes longer to notice, because the symptom is a camera that
 * stops producing frames rather than one that never starts.
 *
 * The cost is that the panel shows the table's defaults rather than what the
 * sensor actually holds, until a control is touched. That is the honest state:
 * this code does not know what the sensor holds.
 */
static bool    g_dirty[CTRL_COUNT];
static bool    g_primed = false;

static void prime(void)
{
    int i;
    if (g_primed) {
        return;
    }
    /* Every row, not just the live ones: the arrays are sized for the whole
       table and an uninitialised entry would be read by nothing but would be
       there waiting if the switch is ever turned on at runtime. */
    for (i = 0; i < CTRL_COUNT; i++) {
        g_value[i] = k_ctrls[i].def;
        g_dirty[i] = false;
    }
    g_primed = true;
}

/* --------------------------------- Accessors ------------------------------- */
int cam_controls_count(void)
{
    return ACTIVE_COUNT;
}

const cam_ctrl_t *cam_controls_at(int i)
{
    if (i < 0 || i >= CTRL_COUNT) {
        return NULL;
    }
    return &k_ctrls[i];
}

int16_t cam_controls_value(int i)
{
    prime();
    if (i < 0 || i >= CTRL_COUNT) {
        return 0;
    }
    return g_value[i];
}

/* ---------------------------------- Apply ---------------------------------- */
static void write_one(int i)
{
    arducam_reg_write(&arducam_mega.sensor, k_ctrls[i].reg,
                      (uint8_t)g_value[i]);
}

void cam_controls_apply_all(void)
{
    int i;
    prime();
    for (i = 0; i < ACTIVE_COUNT; i++) {
        /* Only what was asked for. See the note on g_dirty. */
        if (g_dirty[i]) {
            write_one(i);
        }
    }
}

/**
 * Pull an unsigned decimal out of a query slice, matching @p key at a parameter
 * boundary. Same shape as the one in the servers, kept local so this file has
 * no dependency on either of them.
 */
static bool query_u32(const char *q, size_t qlen, const char *key, uint32_t *out)
{
    size_t klen = strlen(key);
    size_t i;

    for (i = 0; i + klen + 1 <= qlen; i++) {
        if ((i == 0 || q[i - 1] == '&') &&
            memcmp(q + i, key, klen) == 0 && q[i + klen] == '=') {
            size_t   p     = i + klen + 1;
            uint32_t value = 0;
            bool     digit = false;

            while (p < qlen && q[p] >= '0' && q[p] <= '9') {
                value = value * 10u + (uint32_t)(q[p] - '0');
                digit = true;
                p++;
                if (value > 0xFFFFu) {
                    return false;
                }
            }
            if (!digit) {
                return false;
            }
            *out = value;
            return true;
        }
    }
    return false;
}

int cam_controls_apply_query(const char *q, size_t qlen, bool *refused)
{
    int i, applied = 0;

    prime();
    if (refused) {
        *refused = false;
    }

    for (i = 0; i < ACTIVE_COUNT; i++) {
        uint32_t raw;
        int16_t  v;

        if (!query_u32(q, qlen, k_ctrls[i].name, &raw)) {
            continue;
        }

        v = (int16_t)raw;
        if (v < k_ctrls[i].min || v > k_ctrls[i].max) {
            /*
             * Clamp rather than ignore, and say so. A slider that snaps back
             * with no explanation reads as a broken page; the 409 lets the page
             * put "the sensor refused that" on screen while still showing what
             * the hardware actually has.
             */
            v = (v < k_ctrls[i].min) ? k_ctrls[i].min : k_ctrls[i].max;
            if (refused) {
                *refused = true;
            }
        }

        g_value[i] = v;
        g_dirty[i] = true;
        write_one(i);
        printf("cam ctrl %s = %d (reg 0x%04X)\n",
               k_ctrls[i].name, (int)v, (unsigned)k_ctrls[i].reg);
        applied++;
    }

    return applied;
}

/* ----------------------------------- JSON ---------------------------------- */
int cam_controls_json_descriptors(char *buf, size_t cap)
{
    size_t used = 0;
    int    i;
    int    n;

    prime();

    n = snprintf(buf, cap, "[");
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    used = (size_t)n;

    for (i = 0; i < ACTIVE_COUNT; i++) {
        n = snprintf(buf + used, cap - used,
                     "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"%s\","
                     "\"min\":%d,\"max\":%d}",
                     i ? "," : "",
                     k_ctrls[i].name, k_ctrls[i].label, k_ctrls[i].group,
                     (int)k_ctrls[i].min, (int)k_ctrls[i].max);
        if (n < 0 || (size_t)n >= cap - used) {
            return -1;
        }
        used += (size_t)n;
    }

    n = snprintf(buf + used, cap - used, "]");
    if (n < 0 || (size_t)n >= cap - used) {
        return -1;
    }
    return (int)(used + (size_t)n);
}

int cam_controls_json_values(char *buf, size_t cap)
{
    size_t used = 0;
    int    i;

    prime();

    for (i = 0; i < ACTIVE_COUNT; i++) {
        int n = snprintf(buf + used, cap - used, "%s\"%s\":%d",
                         i ? "," : "", k_ctrls[i].name, (int)g_value[i]);
        if (n < 0 || (size_t)n >= cap - used) {
            return -1;
        }
        used += (size_t)n;
    }
    return (int)used;
}
