#!/usr/bin/env bash
# Host tests for the HTTP request parser. No board, no Pico SDK.
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
echo "== host run"
"$out/http_req_test"

if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "== arm cross-compile (size only, not linked)"
    arm-none-eabi-gcc -std=c99 $WARN -Os -mcpu=cortex-m33 -mthumb -c \
        "$src/http/http_req.c" -o "$out/http_req.arm.o"
    arm-none-eabi-size "$out/http_req.arm.o"
else
    echo "== arm-none-eabi-gcc not on PATH, cross-compile skipped"
fi
