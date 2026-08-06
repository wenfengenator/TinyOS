#!/bin/sh
# Build a static-musl MicroPython Unix binary for TinyOS.
set -eu

MICROPY_VERSION=${MICROPY_VERSION:-v1.26.1}
MICROPY_COMMIT=${MICROPY_COMMIT:-647c8b96cae7e202c7a020395b7cfe65e5b8ce04}
MICROPY_ROOT=${MICROPY_ROOT:-/tmp/tinyos-micropython}
WORK_ROOT=${WORK_ROOT:-/tmp/tinyos-micropython-src}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
REPO_ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

rm -rf "$MICROPY_ROOT" "$WORK_ROOT"
git clone --depth 1 --branch "$MICROPY_VERSION" \
    https://github.com/micropython/micropython.git "$WORK_ROOT"
test "$(git -C "$WORK_ROOT" rev-parse HEAD)" = "$MICROPY_COMMIT"

make -C "$WORK_ROOT/mpy-cross" -j"$JOBS"
make -C "$WORK_ROOT/ports/unix" VARIANT=minimal -j"$JOBS" \
    USER_C_MODULES="$REPO_ROOT/programs" \
    CC="gcc -specs=$REPO_ROOT/build/musl-gcc.specs" \
    CFLAGS_EXTRA="-Os -fno-pie" \
    LDFLAGS_EXTRA="-static -no-pie"

mkdir -p "$MICROPY_ROOT"
cp "$WORK_ROOT/ports/unix/build-minimal/micropython" "$MICROPY_ROOT/micropython"
"$MICROPY_ROOT/micropython" -c \
    "print('MicroPython TinyOS bootstrap OK')"
file "$MICROPY_ROOT/micropython"