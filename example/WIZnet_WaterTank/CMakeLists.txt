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

# ---------------------------------------------------------------------------
# The same sources again, with the work split across the two cores.
#
# One source tree, two images, the way the exhibition example does it: the
# detector, the web server and the alert client are the same object code in
# both, and the only difference is a -D. A fix to detection reaches both, and a
# difference on the console came from the split rather than from a build that
# drifted.
#
#   watertank        one core does everything. Kept as the baseline.
#   watertank_dual   core 0 the microphone; core 1 the network and the camera.
#
# The single-core image loses six to eight seconds of audio to every alert it
# sends, measured. That is the number this second target exists to take to zero.
# ---------------------------------------------------------------------------

set(DUAL_NAME watertank_dualcore)

add_executable(${DUAL_NAME}
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        ${CMAKE_CURRENT_LIST_DIR}/webserver.c
        ${CMAKE_CURRENT_LIST_DIR}/settings.c
        ${CMAKE_CURRENT_LIST_DIR}/discord.c
        ${CMAKE_CURRENT_LIST_DIR}/goertzel.c
        ${CMAKE_CURRENT_LIST_DIR}/detector.c
        ${TOE_DIR}/arducam_mega/arducam_mega.c
        )

# Core 1 gets its stack from main RAM, not from the SDK's reserved block.
#
# PICO_CORE1_STACK_SIZE defaults to 0x800 and the SDK places that array in
# SCRATCH_X, which is 4 KB in total - so core 1 cannot be given more than that
# this way, and by default gets exactly 2 KB. Core 0 is not symmetric with it:
# its stack pointer starts at the top of SCRATCH_Y, so it has the whole 4 KB,
# which is how the single-core image ran a TLS handshake at all.
#
# Zero here tells the SDK to reserve nothing, and main.c hands core 1 a 16 KB
# array instead. mbedTLS carries kilobytes of locals through its bignum and
# certificate paths, and a stack overflow next to other RAM does not announce
# itself - it hangs, or corrupts something that fails later somewhere else.
target_compile_definitions(${DUAL_NAME} PRIVATE
        USE_CORE1=1
        PICO_CORE1_STACK_SIZE=0
        )

pico_generate_pio_header(${DUAL_NAME} ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)
pico_generate_pio_header(${DUAL_NAME} ${TOE_DIR}/image_mega.pio)

target_include_directories(${DUAL_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${TOE_DIR}
        ${TOE_DIR}/arducam_mega
        )

target_link_libraries(${DUAL_NAME} PRIVATE
        pico_stdlib
        pico_multicore
        hardware_spi
        hardware_dma
        hardware_i2c
        hardware_pio
        hardware_pwm
        hardware_gpio
        hardware_flash
        hardware_sync
        hardware_watchdog
        ETHERNET_FILES
        IOLIBRARY_FILES
        DNS_FILES
        WIZ_CLAW_FILES
        )

pico_enable_stdio_usb(${DUAL_NAME} 1)
pico_enable_stdio_uart(${DUAL_NAME} 0)

pico_add_extra_outputs(${DUAL_NAME})
