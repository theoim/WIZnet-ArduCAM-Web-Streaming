# Minimal HTTPS client for the W6300.
#
# Lifted from WIZnet-PICO-C-CHIP-TEST/port/wiz-claw, which is an agent that
# talks to Telegram and OpenAI. A Discord webhook needs one POST and nothing
# else, so only the transport came across:
#
#   wiz_claw.h             error codes and log macros
#   wiz_claw_net.c/.h      DNS - discord.com has to become an address
#   wiz_claw_http.c/.h     URL parsing, TCP, request and response
#   wiz_claw_tls.c/.h      handshake and record layer over an ioLibrary socket
#   wiz_claw_ca_bundle.h   trust anchors
#
# Left behind: the agent loop, the LLM client, the Telegram client, and cJSON.
# cJSON alone is 3,470 lines and a webhook body is one field - snprintf covers
# it. 1,596 lines came over out of 5,940.
#
# The CA bundle already carries WE1, the Google Trust Services intermediate
# that signs discord.com, because Telegram sits behind the same issuer. That is
# luck rather than design, so if a handshake ever starts failing with a
# certificate error, regenerating the bundle is the first thing to try.

add_library(WIZ_CLAW_FILES STATIC)

target_sources(WIZ_CLAW_FILES PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/src/wiz_claw_net.c
        ${CMAKE_CURRENT_LIST_DIR}/src/wiz_claw_http.c
        ${CMAKE_CURRENT_LIST_DIR}/src/wiz_claw_tls.c
        )

target_include_directories(WIZ_CLAW_FILES PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
        )

target_link_libraries(WIZ_CLAW_FILES PUBLIC
        pico_stdlib
        pico_rand              # RP2350 TRNG seeds CTR-DRBG
        ETHERNET_FILES
        IOLIBRARY_FILES
        DNS_FILES
        DHCP_FILES             # wiz_claw_net.c offers a DHCP path too
        mbedtls
        mbedx509
        mbedcrypto
        )
