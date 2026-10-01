# Basement water tank monitor.
#
# The camera driver is compiled from the TOE streaming example rather than
# copied, so a fix there reaches here. The microphone chain, the detector and
# the Discord client came across as files because they are finished - each was
# validated on its own before being brought in, and a copy that is not going to
# change is clearer than a dependency that is.

set(TARGET_NAME watertank)
set(TOE_DIR ${CMAKE_CURRENT_LIST_DIR}/../WIZnet_ArduCAMMega_TOE_Web_Streaming)

add_executable(${TARGET_NAME}
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        ${CMAKE_CURRENT_LIST_DIR}/webserver.c
        ${CMAKE_CURRENT_LIST_DIR}/settings.c
        ${CMAKE_CURRENT_LIST_DIR}/discord.c
        ${CMAKE_CURRENT_LIST_DIR}/goertzel.c
        ${CMAKE_CURRENT_LIST_DIR}/detector.c
        ${TOE_DIR}/arducam_mega/arducam_mega.c
        )

pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)
pico_generate_pio_header(${TARGET_NAME} ${TOE_DIR}/image_mega.pio)

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${TOE_DIR}
        ${TOE_DIR}/arducam_mega
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_spi
        hardware_dma
        hardware_i2c
        hardware_pio
        hardware_pwm
        hardware_gpio
        hardware_flash
        hardware_watchdog
        ETHERNET_FILES
        IOLIBRARY_FILES
        DNS_FILES
        WIZ_CLAW_FILES
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
