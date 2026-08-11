#include "psram.h"
#include <pico/stdlib.h>
#include "hardware/clocks.h"
uint8_t psram_reset_en_cmd[] = {
    0x66,   // Reset enable command
    0x99,   // Reset command
};


void psram_spi_init(){

    spi_deinit(spi0);
    spi_init (spi0, 60*1000000);  //60/2  = 30Mhz
    gpio_set_function (PSRAM_PIN_SCLK, GPIO_FUNC_SPI);
    gpio_set_function (PSRAM_PIN_MISO,  GPIO_FUNC_SPI);
    gpio_set_function (PSRAM_PIN_MOSI, GPIO_FUNC_SPI);
    
    gpio_set_drive_strength(PSRAM_PIN_CS, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PSRAM_PIN_SCLK, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PSRAM_PIN_MOSI, GPIO_DRIVE_STRENGTH_4MA);
    gpio_init(PSRAM_PIN_CS);
    gpio_set_dir(PSRAM_PIN_CS, GPIO_OUT);
    gpio_put(PSRAM_PIN_CS, 1);
    psram_reset();
}

/*
    int __not_in_flash_func(spi_write_blocking)(spi_inst_t *spi, const uint8_t *src, size_t len)
*/

void psram_reset(){
    gpio_put(PSRAM_PIN_CS, 0);
    spi_write_blocking(spi0, psram_reset_en_cmd,1); 
    gpio_put(PSRAM_PIN_CS, 1);
    gpio_put(PSRAM_PIN_CS, 0);
    spi_write_blocking(spi0, psram_reset_en_cmd+1,1); 
    gpio_put(PSRAM_PIN_CS, 1);
}
/*
int __not_in_flash_func(spi_read_blocking)(spi_inst_t *spi, uint8_t repeated_tx_data, uint8_t *dst, size_t len) 
*/

void psram_fast_read(uint32_t address, uint8_t *buffer, uint32_t length){
    uint8_t read_cmd[5] = {
        0x0B, //read command
        (address>>16)&0xFF,  // address 16:23
        (address>>8)&0xFF,   // address 8:15
        (address)&0xFF,       // address 0:7
        0x00,                 // dummy wait cycles
    };
    gpio_put(PSRAM_PIN_CS, 0);
    spi_write_blocking(spi0, read_cmd,5); 
    spi_read_blocking(spi0, 0x00, buffer, length );
    gpio_put(PSRAM_PIN_CS, 1);
}


void psram_fast_write(uint32_t address, uint8_t *buffer, uint32_t length){
    uint8_t read_cmd[4] = {
        0x02, //write command
        (address>>16)&0xFF,  // address 16:23
        (address>>8)&0xFF,   // address 8:15
        (address)&0xFF,       // address 0:7
    };
    gpio_put(PSRAM_PIN_CS, 0);
    spi_write_blocking(spi0, read_cmd,4); 
    spi_write_blocking(spi0, buffer, length );
    gpio_put(PSRAM_PIN_CS, 1);
}