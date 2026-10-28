#ifndef BUILD_STAMP_H
#define BUILD_STAMP_H

/*
 * Which image is running. Written into the build directory by
 * tools/build_stamp.cmake on every build, so the id follows the sources
 * without re-running cmake: "<commit>" when this example matches the commit,
 * "<commit>+<hash>" when it has uncommitted changes.
 */
extern const char cam_build_id[];
extern const char cam_build_utc[];

#define CAM_BUILD_GIT cam_build_id
#define CAM_BUILD_UTC cam_build_utc

#endif
