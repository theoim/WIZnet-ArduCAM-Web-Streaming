
#include "arducam_mega.h"
#ifdef USBCDC
#include "tusb.h"
#endif
#include "image_mega.pio.h"
#include <stdlib.h>
#include "string.h"

const int PIN_CAM_SIOC = 1;
const int PIN_CAM_SIOD = 0;
const int PIN_CAM_RESETB = 1;
const int PIN_CAM_PWDN = 27;
const int PIN_CAM_XCLK = -1;
const int PIN_CAM_VSYNC = 4;
const int PIN_CAM_Y2_PIO_BASE = 5;

uint8_t image_buff[1024 * 200];

void spi_slave_init(spi_inst_t *spi);

uint8_t arducam_reg_read(sensor_info_t *config, uint16_t reg)
{
    uint8_t data[2];
    uint8_t length;
    switch (config->sccb_mode)
    {
    case I2C_MODE_16_8:
        data[0] = (uint8_t)(reg >> 8) & 0xFF;
        data[1] = (uint8_t)(reg)&0xFF;
        length = 2;
        break;
    }

    i2c_write_blocking(config->sccb, config->sensor_address, data, length, false);
    
    uint8_t value;
    i2c_read_blocking(config->sccb, config->sensor_address, &value, 1, false);

    return value;
}

uint8_t arducam_reg_write(sensor_info_t *config, uint16_t reg, uint8_t value)
{
    uint8_t data[3];
    uint8_t length = 0;
    switch (config->sccb_mode)
    {
    case I2C_MODE_16_8:
        data[0] = (uint8_t)(reg >> 8) & 0xFF;
        data[1] = (uint8_t)(reg)&0xFF;
        data[2] = value;
        length = 3;
        break;
    }
    int ret = i2c_write_blocking(config->sccb, config->sensor_address, data, length, false);
    return (ret == length) ? 0 : 1;
}



int set_framesize(res_t res)
{
    int ret = 0;
    arducam_mega.frame.res = res;
    switch (res){
        case RES_320X240:
             ret = arducam_reg_write(&(arducam_mega.sensor), RESOLUTION_REG, 0x01);
             sleep_ms(10);
             ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_CLK_DIV_REG, 0x02);
             sleep_ms(10);
             ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_PLL_DIV_REG, 0x01);
        break;
        case RES_640X480:
             ret = arducam_reg_write(&(arducam_mega.sensor), RESOLUTION_REG, 0x02);
             sleep_ms(10);
             ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_CLK_DIV_REG, 0x02);
             sleep_ms(10);
             ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_PLL_DIV_REG, 0x01);
        break;
        case RES_1280X720:
            ret = arducam_reg_write(&(arducam_mega.sensor), RESOLUTION_REG, 0x03);
            sleep_ms(10);
            ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_CLK_DIV_REG, 0x02);
            sleep_ms(10);
            ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_PLL_DIV_REG, 0x01);
        break;
        case RES_1600X1200:
            ret = arducam_reg_write(&(arducam_mega.sensor), RESOLUTION_REG, 0x04);
            sleep_ms(100);
            ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_CLK_DIV_REG, 0x02);
            sleep_ms(100);
             ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_PLL_DIV_REG, 0x01);
        break;
        case RES_1920X1080:
            ret = arducam_reg_write(&(arducam_mega.sensor), RESOLUTION_REG, 0x05);
            sleep_ms(100);
            ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_CLK_DIV_REG, 0x02);
            sleep_ms(100);
            ret += arducam_reg_write(&(arducam_mega.sensor), SYSTEM_PLL_DIV_REG, 0x01);
        break;
        default:
            arducam_mega.frame.res = RES_320X240;
            ret = -1;
        break;
    }

    if(arducam_mega.frame.pixfmt == PIXFORMAT_JPEG){
        arducam_mega.frame.line.length = 512;
    }else{
        switch(arducam_mega.frame.res){
            case RES_320X240:
                arducam_mega.frame.line.length = 320*2;
            break;
            case RES_640X480:
                arducam_mega.frame.line.length = 640*2;
            break;
            case RES_1280X720:
                arducam_mega.frame.line.length = 1280*2;
            break;
            case RES_1600X1200:
                arducam_mega.frame.line.length = 1600*2;
            break;
            case RES_1920X1080:
                arducam_mega.frame.line.length = 1920*2;
            break;
        }
    }
    return ret;
}

int set_pixformat(pixfmt_t pixformat)
{
    int ret = 0;
    arducam_mega.frame.pixfmt = pixformat;
    switch (pixformat) {
    case PIXFORMAT_JPEG:
        ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x01);
        break;
    case PIXFORMAT_RGB565:
         ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x02);
        break;
    case PIXFORMAT_YUV422:
        ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x03);
        break;
    default:
        ret = arducam_reg_write(&(arducam_mega.sensor), PIXEL_FMT_REG, 0x01);
        arducam_mega.frame.pixfmt = PIXFORMAT_JPEG;
        break;
    }

    return ret;
}

int reset()
{
    int ret;
    ret = arducam_reg_write(&(arducam_mega.sensor), CAMERA_RST_REG, 0x00);
    ret += arducam_reg_write(&(arducam_mega.sensor), CAMERA_RST_REG, 0x01);
    sleep_ms(1000);
    return ret;
}



void arducam_sensor_init(sensor_info_t *config){
    gpio_set_function(config->pin_sioc, GPIO_FUNC_I2C);
    gpio_set_function(config->pin_siod, GPIO_FUNC_I2C);
    
    gpio_pull_up(config->pin_sioc);
    gpio_pull_up(config->pin_siod);
    i2c_init(config->sccb, 200 * 1000);

    gpio_init(config->pin_vsync);
    gpio_set_dir(config->pin_vsync, GPIO_IN);
    gpio_pull_up(config->pin_vsync);
    
    gpio_put(config->pin_pwdn, 0);
    sleep_ms(200);

    printf("Resetting camera...\n");
    reset();
    sleep_ms(500);
    
    printf("Setting JPEG format...\n");
    set_pixformat(PIXFORMAT_JPEG);
    sleep_ms(200);
    
    printf("Setting initial resolution to 320x240...\n");
    set_framesize(RES_320X240);
    sleep_ms(200);

    printf("Camera sensor initialization complete\n");
}



static inline void arducam_start_line_dma(void) {
    dma_channel_set_trans_count(arducam_mega.pio_receiver.dma_channel,
                                arducam_mega.frame.line.length / 4, false);
    dma_channel_set_write_addr(arducam_mega.pio_receiver.dma_channel,
                               arducam_mega.frame.line.buffer, true);
}



void arducam_pio_init(pio_receiver_t *config){
    config->dma_channel = dma_claim_unused_channel(true);

    uint offset = pio_add_program(config->pio, &image_program);

    pio_sm_config c = image_program_get_default_config(offset);

    sm_config_set_in_pins(&c, config->pin_y2_pio_base);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_set_consecutive_pindirs(config->pio, config->pio_sm,
                                   config->pin_y2_pio_base, 10, false);

    uint gpio_func;
    if (config->pio == pio0) {
        gpio_func = GPIO_FUNC_PIO0;
    } else if (config->pio == pio1) {
        gpio_func = GPIO_FUNC_PIO1;
    } else {
        gpio_func = GPIO_FUNC_PIO0;
    }

    for (int i = 0; i < 10; i++) {
        gpio_set_function(config->pin_y2_pio_base + i, gpio_func);
    }

    pio_sm_init(config->pio, config->pio_sm, offset, &c);
    pio_sm_set_enabled(config->pio, config->pio_sm, true);

    config->dma_config = dma_channel_get_default_config(config->dma_channel);
    channel_config_set_read_increment(&config->dma_config, false);
    channel_config_set_write_increment(&config->dma_config, true);
    channel_config_set_dreq(&config->dma_config,
                            pio_get_dreq(config->pio, config->pio_sm, false));
    channel_config_set_transfer_data_size(&config->dma_config, DMA_SIZE_32);
    channel_config_set_high_priority(&config->dma_config, true);

    dma_channel_configure(
        config->dma_channel, &config->dma_config,
        arducam_mega.frame.line.buffer,
        &config->pio->rxf[config->pio_sm],
        arducam_mega.frame.line.length / 4,
        false
    );

    printf("[arducam pio_sm=%u, dma_ch=%d, pio_off=%u]\n",
           config->pio_sm, config->dma_channel, offset);
}
int arducam_capture_frame() {
    uint32_t base_adress = 0;
    arducam_mega.frame.line.num = 0;
    arducam_mega.frame.frame_length = 0;
    bool jpeg_header_found = false;
    int retry_count = 0;
    const int max_retries = 3;
    
    // Timeout constants
    const uint32_t VSYNC_TIMEOUT_US = 2000000;  // 2 seconds
    const uint32_t DMA_TIMEOUT_US = 500000;     // 500ms per line
    absolute_time_t timeout_start;

retry_capture:
    arducam_mega.frame.line.num = 0;
    arducam_mega.frame.frame_length = 0;
    jpeg_header_found = false;

    // Wait for VSYNC to go LOW (with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(arducam_mega.sensor.pin_vsync) == true) {
        if (absolute_time_diff_us(timeout_start, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            printf("[ERR] VSYNC timeout waiting for LOW\n");
            return -2;
        }
        tight_loop_contents();
    }
    
    // Wait for VSYNC to go HIGH (frame start, with timeout)
    timeout_start = get_absolute_time();
    while (gpio_get(arducam_mega.sensor.pin_vsync) == false) {
        if (absolute_time_diff_us(timeout_start, get_absolute_time()) > VSYNC_TIMEOUT_US) {
            printf("[ERR] VSYNC timeout waiting for HIGH\n");
            return -2;
        }
        tight_loop_contents();
    }
    
    // Capture frame line by line
    do {
        pio_sm_clear_fifos(arducam_mega.pio_receiver.pio, arducam_mega.pio_receiver.pio_sm);
        arducam_start_line_dma();
        
        // Wait for DMA completion with timeout
        timeout_start = get_absolute_time();
        while (dma_channel_is_busy(arducam_mega.pio_receiver.dma_channel)) {
            if (absolute_time_diff_us(timeout_start, get_absolute_time()) > DMA_TIMEOUT_US) {
                printf("[ERR] DMA timeout on line %d\n", arducam_mega.frame.line.num);
                dma_channel_abort(arducam_mega.pio_receiver.dma_channel);
                return -3;
            }
            tight_loop_contents();
        }
        
        arducam_mega.frame.line.flag_end = 0;
        
        // Check for JPEG header in first few lines (JPEG mode only)
        if (arducam_mega.frame.line.num == 0) {
            if (arducam_mega.frame.line.buffer[0] != 0xFF || arducam_mega.frame.line.buffer[1] != 0xD8) {
                retry_count++;
                if (retry_count < max_retries) goto retry_capture;
                else goto badframe;
            }
        }

        // Set flag for first line
        if(arducam_mega.frame.line.num == 0) {
            arducam_mega.frame.line.flag_top = 1;
        } else {
            arducam_mega.frame.line.flag_top = 0;
        }
        
        // Copy line to image buffer
        memcpy(image_buff + arducam_mega.frame.line.num * arducam_mega.frame.line.length, 
               arducam_mega.frame.line.buffer, arducam_mega.frame.line.length);
        arducam_mega.frame.line.num++;
        
        // Safety check: prevent buffer overflow
        if(arducam_mega.frame.line.num * arducam_mega.frame.line.length >= sizeof(image_buff)) {
            printf("[ERR] Buffer overflow prevented at line %d\n", arducam_mega.frame.line.num);
            goto badframe;
        }
        
    } while(gpio_get(arducam_mega.sensor.pin_vsync) == true);
    
    // Frame capture completed
    arducam_mega.frame.line.flag_end = 1;
    arducam_mega.frame.frame_length = arducam_mega.frame.line.num * arducam_mega.frame.line.length;
    
    return 0;

badframe:
    printf("[ERR] Bad frame captured\n");
    return -1;
}

void arducam_mega_init(){
    arducam_sensor_init(&(arducam_mega.sensor));
    arducam_pio_init(&(arducam_mega.pio_receiver));
}

void arducam_mega_handler(){
}

arducam_mega_t arducam_mega = {
    .frame.read_pixel_index = 0,
    .spi_controller = {
        .spi = spi1,
        .cap_sta = 0,
    },
    .sensor={
        .sccb = i2c0,
        .sccb_mode = I2C_MODE_16_8,
        .sensor_address = 0x1F,
        .pin_sioc = PIN_CAM_SIOC,
        .pin_siod = PIN_CAM_SIOD,
        .pin_resetb = PIN_CAM_RESETB,
        .pin_pwdn   = PIN_CAM_PWDN,
        .pin_xclk = PIN_CAM_XCLK,
        .pin_vsync = PIN_CAM_VSYNC,
    },
    .pio_receiver = {
        .pin_y2_pio_base = PIN_CAM_Y2_PIO_BASE,
        .pio = pio0,
        .pio_sm = 0,
        .dma_channel = 0,
    },
    .init = arducam_mega_init,
    .set_frame_size = set_framesize,
    .set_pixel_format = set_pixformat,
    .get_frame =  arducam_capture_frame,
    .mega_handler =arducam_mega_handler,

    .save_image_to_flash = 0,
};
