# ArduCAM MEGA web streaming - exhibition build.
#
# One source tree, two firmware images. Everything a visitor can see is the same
# object code in both; the only difference is which server file is compiled and
# which -D goes with it, so a difference on screen came from the network stack.
#
#   exhibition_toe    NET_STACK_TOE    192.168.11.3
#   exhibition_lwip   NET_STACK_LWIP   192.168.11.5
#
# The camera driver, PSRAM, UART and the PIO program are compiled from the TOE
# streaming example rather than copied, for the same reason the two examples
# there share them: the boards must capture identically or the comparison is
# measuring two cameras instead of two stacks.

set(LWIP_PATH ${PICO_SDK_PATH}/lib/lwip)
set(TOE_DIR   ${CMAKE_CURRENT_LIST_DIR}/../WIZnet_ArduCAMMega_TOE_Web_Streaming)

# Shared by both images.
set(EXHIBITION_COMMON_SRC
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        ${CMAKE_CURRENT_LIST_DIR}/cam_state.c
        ${CMAKE_CURRENT_LIST_DIR}/cam_controls.c
        ${CMAKE_CURRENT_LIST_DIR}/net_load.c
        ${TOE_DIR}/arducam_mega/arducam_mega.c
        ${TOE_DIR}/psram/psram.c
        ${TOE_DIR}/uart/uart.c
        )

set(EXHIBITION_COMMON_INC
        ${CMAKE_CURRENT_LIST_DIR}
        ${TOE_DIR}
        ${TOE_DIR}/arducam_mega
        ${TOE_DIR}/psram
        ${TOE_DIR}/uart
        )

set(EXHIBITION_COMMON_LIBS
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

# ------------------------------------------------------------------ TOE ------
set(TARGET_NAME exhibition_toe)

add_executable(${TARGET_NAME}
        ${EXHIBITION_COMMON_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/server_toe.c
        )

target_compile_definitions(${TARGET_NAME} PRIVATE NET_STACK_TOE=1)
target_include_directories(${TARGET_NAME} PRIVATE ${EXHIBITION_COMMON_INC})
pico_generate_pio_header(${TARGET_NAME} ${TOE_DIR}/image_mega.pio)
target_link_libraries(${TARGET_NAME} PRIVATE ${EXHIBITION_COMMON_LIBS})

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)
pico_add_extra_outputs(${TARGET_NAME})

# ----------------------------------------------------------------- lwIP ------
set(TARGET_NAME exhibition_lwip)

add_executable(${TARGET_NAME}
        ${EXHIBITION_COMMON_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/server_lwip.c
        )

target_compile_definitions(${TARGET_NAME} PRIVATE NET_STACK_LWIP=1)
target_include_directories(${TARGET_NAME} PRIVATE
        ${EXHIBITION_COMMON_INC}
        ${LWIP_PATH}/src/include
        )

# lwipopts.h lives in the port layer.
target_include_directories(pico_lwip INTERFACE ${PORT_DIR}/lwip)

pico_generate_pio_header(${TARGET_NAME} ${TOE_DIR}/image_mega.pio)
target_link_libraries(${TARGET_NAME} PRIVATE
        ${EXHIBITION_COMMON_LIBS}
        LWIP_FILES
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)
pico_add_extra_outputs(${TARGET_NAME})
