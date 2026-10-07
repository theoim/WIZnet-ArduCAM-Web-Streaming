# Basement water tank monitor - LH site, 2026-10-07.
#
# Three images from one source tree. They differ only in a -D, and that is the
# whole point: a threshold tuned on one is tuned on all of them, and a console
# difference between two of them came from the define rather than from a tree
# that drifted.
#
#   watertank_lh            one core does everything. The regression baseline.
#   watertank_lh_dualcore   core 0 the microphone, core 1 the network and camera
#   watertank_lh_dhcp       the same, asking the router for an address
#
# The source list and the library list live in one function below. They used to
# be written out three times, which is a quiet way to ship an image that is
# missing a file somebody added to the other two - the build still succeeds and
# only that one firmware is short a feature.

set(TOE_DIR ${CMAKE_CURRENT_LIST_DIR}/../WIZnet_ArduCAMMega_TOE_Web_Streaming)

# One target, fully wired. The caller adds nothing but compile definitions.
function(add_watertank_target TARGET)
    add_executable(${TARGET}
            ${CMAKE_CURRENT_LIST_DIR}/main.c
            ${CMAKE_CURRENT_LIST_DIR}/webserver.c
            ${CMAKE_CURRENT_LIST_DIR}/settings.c
            ${CMAKE_CURRENT_LIST_DIR}/discord.c
            ${CMAKE_CURRENT_LIST_DIR}/goertzel.c
            ${CMAKE_CURRENT_LIST_DIR}/detector.c
            ${TOE_DIR}/arducam_mega/arducam_mega.c
            )

    pico_generate_pio_header(${TARGET} ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)
    pico_generate_pio_header(${TARGET} ${TOE_DIR}/image_mega.pio)

    target_include_directories(${TARGET} PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}
            ${TOE_DIR}
            ${TOE_DIR}/arducam_mega
            )

    target_link_libraries(${TARGET} PRIVATE
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

    pico_enable_stdio_usb(${TARGET} 1)
    pico_enable_stdio_uart(${TARGET} 0)

    pico_add_extra_outputs(${TARGET})
endfunction()

# ---------------------------------------------------------------------------
# Single core. Kept so a change to detection can be compared against the
# arrangement it was written for.
# ---------------------------------------------------------------------------
add_watertank_target(watertank_lh)

# ---------------------------------------------------------------------------
# Both cores.
#
# PICO_CORE1_STACK_SIZE=0 tells the SDK to reserve nothing in SCRATCH_X, which
# only holds 4 KB and by default gives core 1 just 2 KB - half of what core 0
# gets, on the core that runs mbedTLS. main.c hands it a 16 KB array instead.
# ---------------------------------------------------------------------------
add_watertank_target(watertank_lh_dualcore)
target_compile_definitions(watertank_lh_dualcore PRIVATE
        USE_CORE1=1
        PICO_CORE1_STACK_SIZE=0
        )

# ---------------------------------------------------------------------------
# Both cores, asking the router for an address and falling back to the fixed
# one in config.h when nothing answers.
#
# Which a site wants is not known until somebody is standing in it, and it
# changes when a router is replaced. Two files on a USB stick cost nothing; a
# second trip does.
# ---------------------------------------------------------------------------
add_watertank_target(watertank_lh_dhcp)
target_compile_definitions(watertank_lh_dhcp PRIVATE
        USE_CORE1=1
        PICO_CORE1_STACK_SIZE=0
        NET_USE_DHCP=1
        )
