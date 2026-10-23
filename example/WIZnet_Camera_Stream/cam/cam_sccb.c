#include "cam_sccb.h"

#include "hardware/gpio.h"

/* Long enough for a sensor clock-stretching at 100 kHz, short enough that a
 * missing device is reported in milliseconds rather than hanging the probe. */
#define SCCB_TIMEOUT_US   20000u

static i2c_inst_t *s_i2c;

void sccb_init(i2c_inst_t *i2c, int pin_sda, int pin_scl, uint32_t hz)
{
    s_i2c = i2c;
    i2c_init(i2c, hz);
    gpio_set_function(pin_sda, GPIO_FUNC_I2C);
    gpio_set_function(pin_scl, GPIO_FUNC_I2C);
    gpio_pull_up(pin_sda);
    gpio_pull_up(pin_scl);
}

bool sccb_present(uint8_t addr)
{
    uint8_t dummy;
    return i2c_read_timeout_us(s_i2c, addr, &dummy, 1, false, SCCB_TIMEOUT_US) >= 0;
}

static int xfer_read(uint8_t addr, const uint8_t *idx, size_t idx_len, uint8_t *val)
{
    /* nostop = false on both: the index write is its own transaction. */
    if (i2c_write_timeout_us(s_i2c, addr, idx, idx_len, false, SCCB_TIMEOUT_US)
            != (int)idx_len)
        return -1;
    if (i2c_read_timeout_us(s_i2c, addr, val, 1, false, SCCB_TIMEOUT_US) != 1)
        return -2;
    return 0;
}

int sccb_read8(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return xfer_read(addr, &reg, 1, val);
}

int sccb_read16(uint8_t addr, uint16_t reg, uint8_t *val)
{
    uint8_t idx[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return xfer_read(addr, idx, 2, val);
}

int sccb_write8(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_write_timeout_us(s_i2c, addr, b, 2, false, SCCB_TIMEOUT_US) == 2 ? 0 : -1;
}

int sccb_write16(uint8_t addr, uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, val };
    return i2c_write_timeout_us(s_i2c, addr, b, 3, false, SCCB_TIMEOUT_US) == 3 ? 0 : -1;
}
