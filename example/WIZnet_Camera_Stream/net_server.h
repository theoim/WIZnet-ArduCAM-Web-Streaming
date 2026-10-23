/**
 * @file    net_server.h
 * @brief   The seam between main.c and whichever stack was compiled in.
 *
 * Two implementations, exactly one of which is built:
 *
 *   server_toe.c    NET_STACK_TOE    blocking socket calls on the WIZnet chip
 *   server_lwip.c   NET_STACK_LWIP   lwIP raw-API callbacks over MACRAW
 *
 * The interface is this small on purpose. Everything a visitor can see - the
 * page, the control table, the metrics, the load generator - lives above this
 * line and is the same object code in both images, so anything that differs on
 * screen came from the stack.
 */

#ifndef __NET_SERVER_H__
#define __NET_SERVER_H__

/** Bring up the chip, the addressing and the listeners. Blocks until ready. */
void net_server_init(void);

/**
 * One pass of the server.
 *
 * Called in a tight loop from main(). It must return promptly: on the TOE side
 * it polls each socket, on the lwIP side it drains the receive path and runs
 * the timers. Anything that blocks in here stops the camera as well.
 */
void net_server_service(void);

#endif /* __NET_SERVER_H__ */
