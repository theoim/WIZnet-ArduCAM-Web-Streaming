set(TARGET_NAME lwip_web_streaming)
set(LWIP_PATH ${PICO_SDK_PATH}/lib/lwip)

# The camera driver, PSRAM, UART, and PIO program are shared with the TOE
# example on purpose: the two demos must capture identically so that any
# difference on screen comes from the network stack and nothing else.
set(TOE_DIR ${CMAKE_CURRENT_LIST_DIR}/../WIZnet_ArduCAMMega_TOE_Web_Streaming)

add_executable(${TARGET_NAME}
        main.c
        httpd_stream.c
        ${TOE_DIR}/arducam_mega/arducam_mega.c
        ${TOE_DIR}/psram/psram.c
        ${TOE_DIR}/uart/uart.c
        )

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${LWIP_PATH}/src/include
        ${TOE_DIR}/arducam_mega
        ${TOE_DIR}/psram
        ${TOE_DIR}/uart
        )

pico_generate_pio_header(${TARGET_NAME} ${TOE_DIR}/image_mega.pio)

# lwipopts.h lives in the port layer.
target_include_directories(pico_lwip INTERFACE
        ${PORT_DIR}/lwip
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_spi
        hardware_dma
        hardware_i2c
        hardware_pio
        hardware_pwm
        hardware_gpio
        hardware_uart
        pico_multicore
        ETHERNET_FILES
        IOLIBRARY_FILES
        LWIP_FILES
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
