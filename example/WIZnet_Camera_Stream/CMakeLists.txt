# Camera library: one tree, one sensor driver per camera, one pin map per module.
#
#   cam/cam_sccb.*        SCCB access, 8- and 16-bit register addresses
#   cam/cam_probe.*       bus scan + chip ID of every known part
#   cam/modules/*.h       pin map per module board (XCLK, PWDN, RESET, oscillator)
#   cam/<sensor>/         sensor drivers - added as the streaming build is split out
#
# Pick the module at configure time and keep one build directory per module, so
# an image for one camera is never flashed onto another:
#
#   cmake -S . -B build_cam_lh -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug \
#         -DCAMERA_MODULE=lh_arducam_qb3mp
#
# Targets
#   camera_probe    no network: power the camera, identify it, print over USB.
#   camera_stream   HTTP + MJPEG on the W6300 TOE, http://192.168.11.7/
#                   (app layer from WIZnet_OV2640_Exhibition, camera via cam/)

set(CAMERA_MODULE "lh_arducam_qb3mp" CACHE STRING "Camera module pin map (cam/modules/<name>.h)")
set_property(CACHE CAMERA_MODULE PROPERTY STRINGS
        lh_arducam_qb3mp ov2640_exhibition ov5640_dvp_module)

# Which sensor driver a module needs. A module without a driver yet still
# builds camera_probe; camera_stream is then skipped with a message.
if(CAMERA_MODULE STREQUAL "lh_arducam_qb3mp")
    set(CAMERA_SENSOR arducam_qb)
elseif(CAMERA_MODULE STREQUAL "ov2640_exhibition")
    set(CAMERA_SENSOR ov2640)
else()
    set(CAMERA_SENSOR "")
endif()

if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/cam/modules/${CAMERA_MODULE}.h)
    message(FATAL_ERROR "CAMERA_MODULE=${CAMERA_MODULE}: no cam/modules/${CAMERA_MODULE}.h")
endif()

execute_process(
        COMMAND git describe --always --dirty --abbrev=7
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE CAM_BUILD_GIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
if(NOT CAM_BUILD_GIT)
    set(CAM_BUILD_GIT "no-git")
endif()
string(TIMESTAMP CAM_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

# Phase 2b diagnostic, LH module only: after every frame-size write, read the
# control registers 0x0122-0x012C and 0x0130-0x0134 twice and log them. Read
# only - nothing is written. Off in normal images.
option(CAM_QB_DIAG "Log the Arducam controller's control registers (read only)" OFF)

set(CAM_COMMON_DEFS
        CAM_MODULE_HEADER="modules/${CAMERA_MODULE}.h"
        CAM_BUILD_GIT="${CAM_BUILD_GIT}"
        CAM_BUILD_UTC="${CAM_BUILD_UTC}")
if(CAM_QB_DIAG)
    list(APPEND CAM_COMMON_DEFS CAM_QB_DIAG=1)
    message(STATUS "camera_stream: CAM_QB_DIAG on - control registers logged (read only)")
endif()

# --------------------------------------------------------------- probe -------
add_executable(camera_probe
        ${CMAKE_CURRENT_LIST_DIR}/probe_main.c
        ${CMAKE_CURRENT_LIST_DIR}/cam/cam_sccb.c
        ${CMAKE_CURRENT_LIST_DIR}/cam/cam_probe.c)

target_include_directories(camera_probe PRIVATE ${CMAKE_CURRENT_LIST_DIR}/cam)
target_compile_definitions(camera_probe PRIVATE ${CAM_COMMON_DEFS})
target_link_libraries(camera_probe PRIVATE
        pico_stdlib hardware_i2c hardware_pwm hardware_clocks)

pico_enable_stdio_usb(camera_probe 1)
pico_enable_stdio_uart(camera_probe 0)
pico_add_extra_outputs(camera_probe)

# -------------------------------------------------------------- stream -------
if(CAMERA_SENSOR STREQUAL "")
    message(STATUS "camera_stream: no sensor driver for CAMERA_MODULE=${CAMERA_MODULE} yet - probe only")
else()
    file(GLOB CAM_SENSOR_SRC ${CMAKE_CURRENT_LIST_DIR}/cam/${CAMERA_SENSOR}/*.c)

    add_executable(camera_stream
            ${CMAKE_CURRENT_LIST_DIR}/main.c
            ${CMAKE_CURRENT_LIST_DIR}/cam_state.c
            ${CMAKE_CURRENT_LIST_DIR}/net_load.c
            ${CMAKE_CURRENT_LIST_DIR}/server_toe.c
            ${CMAKE_CURRENT_LIST_DIR}/http/http_req.c
            ${CMAKE_CURRENT_LIST_DIR}/http/conn.c
            ${CMAKE_CURRENT_LIST_DIR}/capture/dvp_capture.c
            ${CMAKE_CURRENT_LIST_DIR}/capture/cam_pipeline.c
            ${CMAKE_CURRENT_LIST_DIR}/cam/cam_sccb.c
            ${CMAKE_CURRENT_LIST_DIR}/cam/cam_ctrl.c
            ${CAM_SENSOR_SRC})

    target_include_directories(camera_stream PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}
            ${CMAKE_CURRENT_LIST_DIR}/http
            ${CMAKE_CURRENT_LIST_DIR}/cam
            ${CMAKE_CURRENT_LIST_DIR}/cam/${CAMERA_SENSOR}
            ${CMAKE_CURRENT_LIST_DIR}/capture)
    target_compile_definitions(camera_stream PRIVATE ${CAM_COMMON_DEFS} NET_STACK_TOE=1)
    pico_generate_pio_header(camera_stream ${CMAKE_CURRENT_LIST_DIR}/capture/dvp_capture.pio)
    target_link_libraries(camera_stream PRIVATE
            pico_stdlib hardware_dma hardware_i2c hardware_pio hardware_pwm
            hardware_gpio hardware_clocks pico_multicore
            ETHERNET_FILES IOLIBRARY_FILES)

    pico_enable_stdio_usb(camera_stream 1)
    pico_enable_stdio_uart(camera_stream 0)
    pico_add_extra_outputs(camera_stream)
endif()

# ------------------------------------------------------------ tls_bench ------
# No network: what HTTPS would cost the camera (AEAD throughput, ECDHE-ECDSA
# handshake, ticket resumption, per-frame ssl_write, heap) - see tls_bench/.
#
# mbedTLS is compiled into this target with its own configuration. The
# repository-wide port/mbedtls/inc/ssl_config.h is shared with the LH field
# build, whose TLS client must not start offering new ciphersuites because a
# benchmark needed them. The global -DMBEDTLS_CONFIG_FILE is therefore removed
# for this directory (camera_probe and camera_stream do not use mbedTLS).
get_directory_property(_cam_defs COMPILE_DEFINITIONS)
list(FILTER _cam_defs EXCLUDE REGEX "^MBEDTLS_CONFIG_FILE=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_cam_defs}")

file(GLOB TLS_BENCH_MBEDTLS_SRC ${MBEDTLS_DIR}/library/*.c)
# bn_mul.h's assembly runs out of registers below -O2 (see the root CMakeLists).
set_source_files_properties(${TLS_BENCH_MBEDTLS_SRC} PROPERTIES COMPILE_OPTIONS "-O2")

# The bench's throwaway key/cert (tls_bench/bench_cert.c) is git-ignored like
# the device key: no private key of any kind is committed. Generate one with
# openssl (P-256 self-signed, DER arrays bench_cert_der/_len and
# bench_key_der/_len) to build this target; the camera targets do not need it.
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/tls_bench/bench_cert.c)
    message(STATUS "tls_bench: tls_bench/bench_cert.c missing - target skipped")
else()
    add_executable(tls_bench
            ${CMAKE_CURRENT_LIST_DIR}/tls_bench/tls_bench.c
            ${CMAKE_CURRENT_LIST_DIR}/tls_bench/bench_cert.c
            ${TLS_BENCH_MBEDTLS_SRC})
    target_include_directories(tls_bench PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/tls_bench
            ${MBEDTLS_DIR}/include
            ${MBEDTLS_DIR}/library)
    target_compile_definitions(tls_bench PRIVATE
            MBEDTLS_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/tls_bench/tls_bench_config.h"
            CAM_BUILD_GIT="${CAM_BUILD_GIT}"
            CAM_BUILD_UTC="${CAM_BUILD_UTC}")
    target_link_libraries(tls_bench PRIVATE pico_stdlib pico_rand hardware_clocks)
    pico_enable_stdio_usb(tls_bench 1)
    pico_enable_stdio_uart(tls_bench 0)
    pico_add_extra_outputs(tls_bench)
endif()

# ---------------------------------------------------- camera_stream_https ----
# camera_stream over TLS on port 443: https://192.168.11.7/
# Same sources with -DSTREAM_HTTPS, plus tls/ and mbedTLS compiled here with
# tls/https_config.h (never the shared ssl_config.h - see tls_bench above).
#
# The device key and certificate are generated, not committed:
#   tls/make_https_cert.sh [ip]   -> tls/https_cert.c (git-ignored)
# and the demo CA they are signed with lives in ~/.camera-stream-ca.
if(NOT CAMERA_SENSOR STREQUAL "")
    if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/tls/https_cert.c)
        message(STATUS "camera_stream_https: tls/https_cert.c missing - run tls/make_https_cert.sh; target skipped")
    else()
        add_executable(camera_stream_https
                ${CMAKE_CURRENT_LIST_DIR}/main.c
                ${CMAKE_CURRENT_LIST_DIR}/cam_state.c
                ${CMAKE_CURRENT_LIST_DIR}/net_load.c
                ${CMAKE_CURRENT_LIST_DIR}/server_toe.c
                ${CMAKE_CURRENT_LIST_DIR}/http/http_req.c
                ${CMAKE_CURRENT_LIST_DIR}/http/conn.c
                ${CMAKE_CURRENT_LIST_DIR}/capture/dvp_capture.c
                ${CMAKE_CURRENT_LIST_DIR}/capture/cam_pipeline.c
                ${CMAKE_CURRENT_LIST_DIR}/cam/cam_sccb.c
                ${CMAKE_CURRENT_LIST_DIR}/cam/cam_ctrl.c
                ${CAM_SENSOR_SRC}
                ${CMAKE_CURRENT_LIST_DIR}/tls/tls_server.c
                ${CMAKE_CURRENT_LIST_DIR}/tls/https_cert.c
                ${TLS_BENCH_MBEDTLS_SRC})
        target_include_directories(camera_stream_https PRIVATE
                ${CMAKE_CURRENT_LIST_DIR}
                ${CMAKE_CURRENT_LIST_DIR}/http
                ${CMAKE_CURRENT_LIST_DIR}/cam
                ${CMAKE_CURRENT_LIST_DIR}/cam/${CAMERA_SENSOR}
                ${CMAKE_CURRENT_LIST_DIR}/capture
                ${CMAKE_CURRENT_LIST_DIR}/tls
                ${MBEDTLS_DIR}/include
                ${MBEDTLS_DIR}/library)
        target_compile_definitions(camera_stream_https PRIVATE
                ${CAM_COMMON_DEFS} NET_STACK_TOE=1 STREAM_HTTPS=1
                MBEDTLS_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/tls/https_config.h")
        pico_generate_pio_header(camera_stream_https ${CMAKE_CURRENT_LIST_DIR}/capture/dvp_capture.pio)
        target_link_libraries(camera_stream_https PRIVATE
                pico_stdlib pico_rand hardware_dma hardware_i2c hardware_pio hardware_pwm
                hardware_gpio hardware_clocks pico_multicore
                ETHERNET_FILES IOLIBRARY_FILES)
        pico_enable_stdio_usb(camera_stream_https 1)
        pico_enable_stdio_uart(camera_stream_https 0)
        pico_add_extra_outputs(camera_stream_https)
    endif()
endif()
