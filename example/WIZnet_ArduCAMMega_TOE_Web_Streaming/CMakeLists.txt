set(TARGET_NAME toe_web_streaming)

add_executable(${TARGET_NAME}
        main.c
        arducam_mega/arducam_mega.c
        psram/psram.c
        uart/uart.c
        )

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/arducam_mega
        ${CMAKE_CURRENT_LIST_DIR}/psram
        ${CMAKE_CURRENT_LIST_DIR}/uart
        )

pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/image_mega.pio)

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
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
