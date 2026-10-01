# INMP441 I2S capture check.
#
# Standalone on purpose: no camera, no Ethernet, no PSRAM. It exists to prove
# the microphone and the PIO divider before any of this goes into the
# exhibition build, so that a problem found later is known not to be here.
#
# It lives in this tree rather than its own so that the board selection, the
# platform and the SDK all come from the same top-level CMakeLists as the
# exhibition images. A divider that is right at 200 MHz and wrong at 150 MHz is
# exactly the kind of thing a separate build would hide.
#
# Pins GP2, GP3, GP4 are the only ones the exhibition build leaves free:
#   GP5..GP14   camera PIO receiver (10 consecutive)
#   GP15..GP22  W6300 QSPI
#   GP26..GP29  camera SPI

set(TARGET_NAME i2s_mic_test)

add_executable(${TARGET_NAME}
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        ${CMAKE_CURRENT_LIST_DIR}/goertzel.c
        ${CMAKE_CURRENT_LIST_DIR}/detector.c
        )

pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_pio
        hardware_pwm
        hardware_dma
        hardware_clocks
        hardware_irq
        )

# Console over USB. GP0/GP1 stay unused so the UART pins remain available when
# this moves into the exhibition firmware.
pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
