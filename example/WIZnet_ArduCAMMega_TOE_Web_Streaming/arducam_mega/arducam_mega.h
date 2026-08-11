#ifndef _ARDUCAM__H
#define _ARDUCAM__H
#include <stdio.h>
#include "string.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/spi.h"
#include "pico/util/queue.h"
#include "pico/multicore.h"
#include "hardware/pwm.h"
#include "mega_ccm_regs.h"

#define CMD_QUEUE_LENGTH  5
#define LINE_QUEUE_LENGTH  6

/*Hardware pin definition*/
#define SPI_MOSI_PIN 27
#define SPI_MISO_PIN 28
#define SPI_SCLK_PIN 26
#define SPI_CS_PIN   29


#define ARDUCHIP_FRAMES     0x01
#define ARDUCHIP_TEST1      0x00 // TEST register
#define ARDUCHIP_FIFO       0x04 // FIFO and I2C control
#define ARDUCHIP_FIFO_2     0x07 // FIFO and I2C control
#define FIFO_CLEAR_ID_MASK  0x01
#define FIFO_START_MASK     0x02

#define FIFO_RDPTR_RST_MASK 0x10
#define FIFO_WRPTR_RST_MASK 0x20
#define FIFO_CLEAR_MASK     0x80

#define ARDUCHIP_TRIG       0x44 // Trigger source
#define VSYNC_MASK          0x01
#define SHUTTER_MASK        0x02
#define CAP_DONE_MASK      (1 << 2 )

#define FIFO_SIZE1          0x45 // Camera write FIFO size[7:0] for burst to read
#define FIFO_SIZE2          0x46 // Camera write FIFO size[15:8]
#define FIFO_SIZE3          0x47 // Camera write FIFO size[18:16]

#define SENSOR_DATA         0x48 // Camera write FIFO size[18:16]

#define BURST_FIFO_READ     0x3C // Burst FIFO read operation
#define SINGLE_FIFO_READ    0x3D // Single FIFO read operation

#define CAM_REG_POWER_CONTROL                      0X02
#define CAM_REG_SENSOR_RESET                       0X07
#define CAM_REG_FORMAT                             0X20
#define CAM_REG_CAPTURE_RESOLUTION                 0X21
#define CAM_REG_BRIGHTNESS_CONTROL                 0X22
#define CAM_REG_CONTRAST_CONTROL                   0X23
#define CAM_REG_SATURATION_CONTROL                 0X24
#define CAM_REG_EV_CONTROL                         0X25
#define CAM_REG_WHILEBALANCE_MODE_CONTROL          0X26
#define CAM_REG_COLOR_EFFECT_CONTROL               0X27
#define CAM_REG_SHARPNESS_CONTROL                  0X28
#define CAM_REG_AUTO_FOCUS_CONTROL                 0X29
#define CAM_REG_IMAGE_QUALITY                      0x2A
#define CAM_REG_EXPOSURE_GAIN_WHILEBALANCE_CONTROL 0X30
#define CAM_REG_MANUAL_GAIN_BIT_9_8                0X31
#define CAM_REG_MANUAL_GAIN_BIT_7_0                0X32
#define CAM_REG_MANUAL_EXPOSURE_BIT_19_16          0X33
#define CAM_REG_MANUAL_EXPOSURE_BIT_15_8           0X34
#define CAM_REG_MANUAL_EXPOSURE_BIT_7_0            0X35
#define CAM_REG_BURST_FIFO_READ_OPERATION          0X3C
#define CAM_REG_SINGLE_FIFO_READ_OPERATION         0X3D
#define CAM_REG_SENSOR_ID                          0x40
#define CAM_REG_YEAR_ID                            0x41
#define CAM_REG_MONTH_ID                           0x42
#define CAM_REG_DAY_ID                             0x43
#define CAM_REG_SENSOR_STATE                       0x44
#define CAM_REG_FPGA_VERSION_NUMBER                0x49
#define CAM_REG_DEBUG_DEVICE_ADDRESS               0X0A
#define CAM_REG_DEBUG_REGISTER_HIGH                0X0B
#define CAM_REG_DEBUG_REGISTER_LOW                 0X0C
#define CAM_REG_DEBUG_REGISTER_VALUE               0X0D
#define CAM_REG_DEBUG_REGISTER_VALUE_H             0X0E


/*reg for flash */

#define FLASH_REG_SAVE            					0X50

#define CAM_I2C_READ_MODE                          (1 << 0)
#define CAM_REG_SENSOR_STATE_IDLE                  (1 << 1)
#define CAM_SENSOR_RESET_ENABLE                    (1 << 6)
#define CAM_FORMAT_BASICS                          (0 << 0)
#define CAM_SET_CAPTURE_MODE                       (0 << 7)
#define CAM_SET_VIDEO_MODE                         (1 << 7)

typedef struct {
uint8_t TEST_REG_VAL;
uint8_t POWER_CONTROL;                      
uint8_t SENSOR_RESET;                       
uint8_t FORMAT ;                            
uint8_t CAPTURE_RESOLUTION ;                
uint8_t BRIGHTNESS_CONTROL;                 
uint8_t CONTRAST_CONTROL ;                  
uint8_t SATURATION_CONTROL ;                
uint8_t EV_CONTROL  ;                       
uint8_t WHILEBALANCE_MODE_CONTROL ;         
uint8_t COLOR_EFFECT_CONTROL;               
uint8_t SHARPNESS_CONTROL   ;               
uint8_t AUTO_FOCUS_CONTROL  ;               
uint8_t IMAGE_QUALITY      ;                
uint8_t EXPOSURE_GAIN_WHILEBALANCE_CONTROL; 
uint8_t MANUAL_GAIN_BIT_9_8  ;              
uint8_t MANUAL_GAIN_BIT_7_0   ;             
uint8_t MANUAL_EXPOSURE_BIT_19_16 ;         
uint8_t MANUAL_EXPOSURE_BIT_15_8  ;         
uint8_t MANUAL_EXPOSURE_BIT_7_0   ;         
uint8_t BURST_FIFO_READ_OPERATION ;         
uint8_t SINGLE_FIFO_READ_OPERATION ;        
uint8_t SENSOR_ID      ;                    
uint8_t YEAR_ID        ;                    
uint8_t MONTH_ID       ;                    
uint8_t DAY_ID          ;                   
uint8_t SENSOR_STATE    ;                   
uint8_t FPGA_VERSION_NUMBER  ;              
uint8_t DEBUG_DEVICE_ADDRESS ;              
uint8_t DEBUG_REGISTER_HIGH  ;              
uint8_t DEBUG_REGISTER_LOW   ;              
uint8_t DEBUG_REGISTER_VALUE  ;             
uint8_t DEBUG_REGISTER_VALUE_H ;            
}spi_regs_t;



typedef struct {
	/*hardware define*/
  	uint8_t sensor_address;
	i2c_inst_t *sccb;
	enum i2c_mode sccb_mode;
	int pin_sioc;
	int pin_siod;
	int pin_resetb;
	int pin_pwdn;
	int pin_xclk;
	int pin_vsync;


}sensor_info_t;

typedef struct  {
	// Y2, Y3, Y4, Y5, Y6, Y7, Y8, PCLK, HREF
	int pin_y2_pio_base;
	PIO pio;
	uint pio_sm;
	uint dma_channel;
	dma_channel_config dma_config;
}pio_receiver_t;


typedef struct {
	queue_t line_queue;
	uint8_t buffer[2592*2];
	uint32_t length;
	uint32_t num;
	uint8_t flag_top;
	uint8_t flag_end;
}line_t;


typedef struct {
	/*image define */
  pixfmt_t pixfmt;
  res_t res;
  uint32_t frame_length;
  line_t line;
  volatile uint32_t read_pixel_index;
}frame_info_t;

typedef enum{
  REV_CMD,
  REV_DAT,
  REV_BURST_READ,
}rev_sta_t;

typedef struct  {
 volatile rev_sta_t rev_sta;
  uint8_t cmd;
  uint8_t data;
} mega_cmd_t;

typedef struct {
  spi_inst_t *spi;
  volatile uint8_t cs_active;
  spi_regs_t mega_regs;
  mega_cmd_t mega_cmd;
  uint8_t cap_sta;  //[Bit[2]: capture done ]
  queue_t mega_cmd_queue;
}spi_controller_t;



typedef struct {
	sensor_info_t sensor;
    pio_receiver_t pio_receiver;
    frame_info_t frame;
    spi_controller_t spi_controller;

    /*sensor config api*/
	int (*set_frame_size)(res_t res);
	int (*set_pixel_format)(pixfmt_t pixfmt);

	/*operation api*/
	void(*init)(void);
	int (*get_frame)(void);
	void (*mega_handler)(void);


	/*flag*/
	volatile uint8_t save_image_to_flash;

} arducam_mega_t;

int save_image_to_msc();

extern arducam_mega_t arducam_mega;
extern volatile uint8_t capture_down;
extern volatile uint8_t http_transfer_down;
extern uint8_t image_buff[];  // External access to image buffer

/* Profiling counters for the last successful capture (see arducam_mega.c). */
extern volatile uint32_t arducam_vsync_wait_us;
extern volatile uint32_t arducam_readout_us;
extern volatile uint32_t arducam_line_count;

/* JPEG parser diagnostics for the last capture. */
extern volatile uint32_t arducam_parse_pos;
extern volatile uint8_t  arducam_in_scan;
extern volatile uint32_t arducam_jpeg_end;

/* Sensor clock dividers - the main lever on frame rate. */
extern volatile uint8_t arducam_clk_div;
extern volatile uint8_t arducam_pll_div;
int arducam_apply_clock_div(uint32_t settle_ms);
int arducam_set_clock_div(uint8_t clk_div, uint8_t pll_div);
int arducam_recover(void);
void mega_cmd_process_fun();
// void arducam_reg_write(sensor_info_t *config, uint16_t reg, uint8_t value);
// uint8_t arducam_reg_read(sensor_info_t *config, uint16_t reg);
// void arducam_regs_write(sensor_info_t *config, struct sensor_reg* regs_list);

// UC-C14 ArduCAM MEGA 특화 함수들
void diagnose_uc_c14_hardware(void);
void reset_dma_for_capture(void);
void force_uc_c14_sensor_streaming(void);
int arducam_capture_direct_gpio(void);

#endif
