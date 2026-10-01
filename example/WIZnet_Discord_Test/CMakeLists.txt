# Discord webhook over TLS, on its own.
#
# No camera and no microphone: the question here is whether the board can
# complete a TLS handshake and get a 2xx out of Discord. Mixing that with the
# rest would mean a failure could be any of three things.

set(TARGET_NAME discord_test)

add_executable(${TARGET_NAME}
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        ${CMAKE_CURRENT_LIST_DIR}/discord.c
        ${CMAKE_CURRENT_LIST_DIR}/test_image.c
        )

target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_spi
        hardware_dma
        hardware_pio
        ETHERNET_FILES
        IOLIBRARY_FILES
        DNS_FILES
        WIZ_CLAW_FILES
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
