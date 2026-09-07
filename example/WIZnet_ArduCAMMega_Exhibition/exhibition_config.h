/**
 * @file    exhibition_config.h
 * @brief   Build-time selection between the two network stacks, and everything
 *          that differs because of it.
 *
 * One source tree, two firmware images. The CMakeLists in this directory
 * defines exactly one of these:
 *
 *   NET_STACK_TOE    the WIZnet chip terminates TCP        -> exhibition_toe
 *   NET_STACK_LWIP   lwIP terminates TCP on the MCU        -> exhibition_lwip
 *
 * Flash the two images onto two boards, put them side by side, and every
 * difference on screen comes from the stack - the camera driver, the page, the
 * control table and the load generator are the same object code in both.
 *
 * The addresses differ so both boards can sit on one network at once.
 */

#ifndef __EXHIBITION_CONFIG_H__
#define __EXHIBITION_CONFIG_H__

#if defined(NET_STACK_TOE) && defined(NET_STACK_LWIP)
#error "Define exactly one of NET_STACK_TOE / NET_STACK_LWIP"
#endif
#if !defined(NET_STACK_TOE) && !defined(NET_STACK_LWIP)
#error "Define exactly one of NET_STACK_TOE / NET_STACK_LWIP"
#endif

/* --------------------------------- Identity -------------------------------- */
#if defined(NET_STACK_TOE)
  #define STACK_NAME        "TOE"
  #define STACK_BUILD_TAG   "toe " __DATE__ " " __TIME__
  #define NET_MAC_ADDR      {0x00, 0x08, 0xDC, 0x12, 0x34, 0x57}
  #define NET_IP_ADDR       {192, 168, 11, 3}
#else
  #define STACK_NAME        "lwIP"
  #define STACK_BUILD_TAG   "lwip " __DATE__ " " __TIME__
  #define NET_MAC_ADDR      {0x00, 0x08, 0xDC, 0x12, 0x34, 0x20}
  #define NET_IP_ADDR       {192, 168, 11, 6}
#endif

#define NET_SUBNET_MASK     {255, 255, 255, 0}
#define NET_GATEWAY         {192, 168, 11, 1}
#define NET_DNS_ADDR        {8, 8, 8, 8}

#define HTTP_PORT           80

/* --------------------------------- Platform -------------------------------- */
#define PLL_SYS_KHZ         (200 * 1000)

/*
 * TOE only. Four hardware sockets all listen on port 80, because a listening
 * socket becomes the connection: with one listener the page could not hold
 * /stream open and poll /api/status at the same time, and the load generator
 * needs a third.
 *
 *   1 stream + 1 status poll + 1 load + 1 spare for a reload arriving early
 *
 * lwIP does not need this - accept() returns a new pcb and the listener stays
 * open - so the count is not used there.
 */
#define HTTP_SOCK_COUNT     4
#define SOCK_HTTP_BASE      0

/* ---------------------------- Sensor controls ------------------------------ */
/*
 * OFF by default, and that is a decision rather than a placeholder.
 *
 * The control table in cam_controls.c writes ArduCAM MEGA CCM registers whose
 * ranges have not been checked against a module. The first version of this
 * build pushed the whole table into the sensor at boot and both images got
 * slower - a plain-value register written with a value that means something
 * else changes what the sensor emits, and JPEG quality is the one that changes
 * how big every frame is.
 *
 * Set this to 1 only after the ranges in cam_controls.c have been verified
 * against the module in front of you. At 0 the sensor is never written to:
 * /api/controls answers an empty list, /api/cam does nothing, and the panel
 * says so. The camera then behaves exactly as it does in the two streaming
 * examples this build came from.
 */
#ifndef EXHIBITION_SENSOR_CONTROLS
#define EXHIBITION_SENSOR_CONTROLS 0
#endif

/* --------------------------------- Capture --------------------------------- */
/*
 * Start capturing at boot instead of waiting for someone to press START.
 *
 * At an exhibition nobody presses START - the board is powered on before the
 * doors open and has to be showing something from then on. It also removes the
 * window where a browser holds a /stream connection open against a device that
 * is not producing frames, which on the lwIP side is a pcb out of a pool of
 * five doing nothing.
 *
 * Set to 0 to get the old behaviour, where the page decides.
 */
#ifndef EXHIBITION_AUTOSTART
#define EXHIBITION_AUTOSTART 1
#endif

#define FAIL_STREAK_RECOVER 10          /* failed captures before a sensor reset */
#define MJPEG_BOUNDARY      "wiznetframe"

/* --------------------------------- Logging --------------------------------- */
/*
 * Print every request line as it is routed. On by default while the lwIP side
 * is being brought up: without it a serial log shows only what the stream did,
 * and the question is whether the status poll ever arrived at all.
 *
 * Set to 0 for the exhibition itself - one printf per request is real work on
 * the side of the build where the MCU is also the TCP stack.
 */
/*
 * Print lwIP pool usage every ten seconds. Only meaningful on the lwIP image.
 *
 * Added because the lwIP side degraded over fifteen minutes - 7.7 fps down to
 * 0.9, network send 11 ms up to 56 - which is the shape of a resource running
 * out rather than of a link being slow. Guessing which one cost several rounds
 * of rebuild-and-flash; asking lwIP costs one line per pool.
 */
#ifndef EXHIBITION_LWIP_STATS
#define EXHIBITION_LWIP_STATS 1
#endif

#ifndef EXHIBITION_HTTP_LOG
#define EXHIBITION_HTTP_LOG 0
#endif

/* ------------------------------- Load generator ---------------------------- */
/*
 * The exhibit. GET /load?kb=N returns N KB of filler, and the page asks for it
 * over and over on one connection while the stream runs.
 *
 * ONE connection on purpose. Loading with more connections would measure the
 * socket count - the TOE has four hardware sockets and lwIP has as many pcbs as
 * memory allows - and the visitor would read "the TOE ran out of sockets" as
 * "the TOE is slower". With a single connection on both boards the only thing
 * that differs is the per-byte cost of the stack, which is the comparison this
 * example exists to make.
 */
#define LOAD_KB_MAX         256u        /* clamp; the page offers 32/64/128     */
#define LOAD_CHUNK_SIZE     1024u       /* filler is emitted a KB at a time     */

#endif /* __EXHIBITION_CONFIG_H__ */
