#ifndef _PSRAM_H
#define _PSRAM_H
#include <stdio.h>
#include "string.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/spi.h"  
#include "hardware/gpio.h"


#define PSRAM_PIN_CS    21
#define PSRAM_PIN_SCLK  22
#define PSRAM_PIN_MISO    20
#define PSRAM_PIN_MOSI    23  
  
void psram_spi_init(void);
void psram_reset(void);
void psram_fast_read(uint32_t address, uint8_t *buffer, uint32_t length);
void psram_fast_write(uint32_t address, uint8_t *buffer, uint32_t length);
#endif