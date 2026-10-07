# OV2640 web streaming - exhibition build.
#
# Same arrangement as WIZnet_ArduCAMMega_Exhibition: one source tree, two
# firmware images, differing by a single -D so that any difference on screen
# came from the network stack and not from the camera or the page.
#
#   ov2640_toe     NET_STACK_TOE    hardware TCP/IP in the W6300
#   ov2640_lwip    NET_STACK_LWIP   software TCP/IP on the RP2350
#
# What is different from that example:
#
#   The camera driver is a copy, not a shared compile. The Mega example pulls
#   arducam_mega.c out of the TOE streaming example so both boards capture with
#   identical object code - which is the right call when the only variable is
#   meant to be the network stack. Here the sensor itself is the change, so
#   sharing the file would mean editing the Mega's driver to suit a part it does
#   not have. ov2640.c is this example's own.
#
#   No mic_i2s and no psram. The microphone belongs to the water tank work, and
#   psram.c claims GP20-23, which is the W6300's QSPI bus - it is never called
#   today and it is not going to start being called from here.

set(LWIP_PATH ${PICO_SDK_PATH}/lib/lwip)

set(OV2640_COMMON_SRC
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        ${CMAKE_CURRENT_LIST_DIR}/cam_state.c
        ${CMAKE_CURRENT_LIST_DIR}/cam_controls.c
        ${CMAKE_CURRENT_LIST_DIR}/net_load.c
        ${CMAKE_CURRENT_LIST_DIR}/ov2640.c
        ${CMAKE_CURRENT_LIST_DIR}/ov2640_regs.c
        )

set(OV2640_COMMON_INC
        ${CMAKE_CURRENT_LIST_DIR}
        )

set(OV2640_COMMON_LIBS
        pico_stdlib
        hardware_dma
        hardware_i2c
        hardware_pio
        hardware_pwm
        hardware_gpio
        hardware_clocks
        pico_multicore
        ETHERNET_FILES
        IOLIBRARY_FILES
        )

# ------------------------------------------------------------------ TOE ------
set(TARGET_NAME ov2640_toe)

add_executable(${TARGET_NAME}
        ${OV2640_COMMON_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/server_toe.c
        )

target_compile_definitions(${TARGET_NAME} PRIVATE NET_STACK_TOE=1)
target_include_directories(${TARGET_NAME} PRIVATE ${OV2640_COMMON_INC})
pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/image_mega.pio)
target_link_libraries(${TARGET_NAME} PRIVATE ${OV2640_COMMON_LIBS})

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)
pico_add_extra_outputs(${TARGET_NAME})

# ----------------------------------------------------------------- lwIP ------
set(TARGET_NAME ov2640_lwip)

add_executable(${TARGET_NAME}
        ${OV2640_COMMON_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/server_lwip.c
        )

target_compile_definitions(${TARGET_NAME} PRIVATE NET_STACK_LWIP=1)
target_include_directories(${TARGET_NAME} PRIVATE
        ${OV2640_COMMON_INC}
        ${LWIP_PATH}/src/include
        )

# lwipopts.h lives in the port layer.
target_include_directories(pico_lwip INTERFACE ${PORT_DIR}/lwip)

pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/image_mega.pio)
target_link_libraries(${TARGET_NAME} PRIVATE
        ${OV2640_COMMON_LIBS}
        LWIP_FILES
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)
pico_add_extra_outputs(${TARGET_NAME})
