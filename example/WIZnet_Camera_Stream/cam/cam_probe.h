/**
 * @file    cam_probe.h
 * @brief   Identify which camera is on the SCCB bus, by address and chip ID.
 */

#ifndef CAM_PROBE_H
#define CAM_PROBE_H

/** Scan the bus, read the ID registers of every known part that answers, and
 *  print the result. Returns the number of known parts identified. */
int cam_probe_report(void);

#endif
