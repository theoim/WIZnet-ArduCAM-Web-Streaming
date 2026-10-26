#!/usr/bin/env bash
# Host tests for the HTTP request parser, connection table, camera controls, settings store and accounts. No board, no Pico SDK.
#
#   example/WIZnet_Camera_Stream/test/run_host_tests.sh
#
# Uses MinGW gcc on Windows (Git Bash) or any gcc/clang elsewhere. The parser
# is also cross-compiled with arm-none-eabi-gcc when that is on PATH, so a
# warning that only the ARM compiler emits is caught here and not on the board.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/.."
out="${TMPDIR:-/tmp}/http_req_test"
mkdir -p "$out"

# The parser ships on the board: strictest warnings. The test file only has to
# be correct C, so it gets the ordinary set (it mixes int and size_t freely).
WARN="-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wstrict-prototypes"
TWARN="-Wall -Wextra -Werror"
# MinGW's default printf lacks %zu; ask for the C99 one. No
# -fstack-protector here: on MinGW 8.1 it reads the canary through %fs (the
# Linux TLS layout) inside the inlined C99 printf and faults on the first call.
# The fuzz test checks sentinels around the context instead.
DEFS="-D__USE_MINGW_ANSI_STDIO=1"

echo "== host build"
gcc -std=c99 $WARN $DEFS -O1 -g -c \
    "$src/http/http_req.c" -o "$out/http_req.o"
gcc -std=c99 $TWARN $DEFS -O1 -g \
    -I"$src/http" "$here/http_req_test.c" "$out/http_req.o" -o "$out/http_req_test"
gcc -std=c99 $WARN $DEFS -O1 -g -c \
    "$src/http/conn.c" -o "$out/conn.o"
gcc -std=c99 $TWARN $DEFS -O1 -g \
    -I"$src/http" "$here/conn_test.c" "$out/conn.o" -o "$out/conn_test"
gcc -std=c99 $WARN $DEFS -O1 -g -c     "$src/cam/cam_ctrl.c" -o "$out/cam_ctrl.o"
gcc -std=c99 $TWARN $DEFS -O1 -g     -I"$src/cam" "$here/cam_ctrl_test.c" "$out/cam_ctrl.o" -o "$out/cam_ctrl_test"
gcc -std=c99 $WARN $DEFS -O1 -g -c "$src/settings/settings_store.c" -o "$out/settings_store.o"
gcc -std=c99 $TWARN $DEFS -O1 -g -I"$src/settings" "$here/settings_store_test.c" "$out/settings_store.o" -o "$out/settings_store_test"
# auth.c against the real mbedTLS PBKDF2 - the library sources the board uses.
mb="$src/../../libraries/mbedtls"
MBDEFS="-DMBEDTLS_CONFIG_FILE=\"host_mbedtls_config.h\" -I$here -I$mb/include -I$mb/library"
for f in md sha256 pkcs5 platform_util platform; do
    gcc -std=c99 $DEFS $MBDEFS -O1 -c "$mb/library/$f.c" -o "$out/mb_$f.o"
done
gcc -std=c99 $WARN $DEFS $MBDEFS -O1 -g -c "$src/auth/auth.c" -o "$out/auth.o"
gcc -std=c99 $TWARN $DEFS $MBDEFS -O1 -g -I"$src/auth" "$here/auth_test.c" "$out/auth.o"     "$out"/mb_md.o "$out"/mb_sha256.o "$out"/mb_pkcs5.o "$out"/mb_platform_util.o "$out"/mb_platform.o -o "$out/auth_test"
gcc -std=c99 $WARN $DEFS -O1 -g -I"$src/auth" -c "$src/auth/session.c" -o "$out/session.o"
gcc -std=c99 $TWARN $DEFS -O1 -g -I"$src/auth" "$here/session_test.c" "$out/session.o" -o "$out/session_test"
echo "== host run"
"$out/http_req_test"
"$out/conn_test"
"$out/cam_ctrl_test"
"$out/settings_store_test"
"$out/auth_test"
"$out/session_test"

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "== arm cross-compile (size only, not linked)"
    arm-none-eabi-gcc -std=c99 $WARN -Os -mcpu=cortex-m33 -mthumb -c \
        "$src/http/http_req.c" -o "$out/http_req.arm.o"
    arm-none-eabi-gcc -std=c99 $WARN -Os -mcpu=cortex-m33 -mthumb -c \
        "$src/http/conn.c" -o "$out/conn.arm.o"
    arm-none-eabi-gcc -std=c99 $WARN -Os -mcpu=cortex-m33 -mthumb -c         "$src/cam/cam_ctrl.c" -o "$out/cam_ctrl.arm.o"
    arm-none-eabi-gcc -std=c99 $WARN -Os -mcpu=cortex-m33 -mthumb -c "$src/settings/settings_store.c" -o "$out/settings_store.arm.o"
    arm-none-eabi-gcc -std=c99 $WARN -Os -mcpu=cortex-m33 -mthumb -I"$src/auth" -c "$src/auth/session.c" -o "$out/session.arm.o"
    arm-none-eabi-size "$out/http_req.arm.o" "$out/conn.arm.o" "$out/cam_ctrl.arm.o" "$out/settings_store.arm.o" "$out/session.arm.o"
else
    echo "== arm-none-eabi-gcc not on PATH, cross-compile skipped"
fi
