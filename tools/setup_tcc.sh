#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

# Build TinyOS's target toolchain (musl libc + TinyCC) entirely from source that
# is vendored in this repo under third_party/.  No network access and no distro
# packages are required — only a standard host toolchain (gcc, make, coreutils).
#
# Outputs (consumed by the Makefile):
#   MUSL_ROOT  musl libc staged in Debian multiarch layout
#              (usr/lib/x86_64-linux-musl/{libc.a,crt*.o,musl-gcc.specs}, headers)
#   TCC_BIN    a static-musl TinyCC binary            ($TCC_ROOT/tcc)
#   TCC_SELF   self-host root (tcc sources + musl runtime + TCCSELF.I/BUILD.RSP)
set -eu

MUSL_VERSION=${MUSL_VERSION:-1.2.4}
# Provenance of third_party/tinycc/tinycc-38059770.tar.gz (repo.or.cz mob rev).
TCC_REV=380597704ee9784442ef3c7ef06e105258a11c5d
TCC_TARBALL=${TCC_TARBALL:-tinycc-38059770.tar.gz}

MUSL_ROOT=${MUSL_ROOT:-/tmp/tinyos-musl}
TCC_ROOT=${TCC_ROOT:-/tmp/tinycc}
TCC_SELF=${TCC_SELF:-/tmp/tcc-selfroot}
WORK_ROOT=${WORK_ROOT:-/tmp/tinyos-toolchain}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
REPO_ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
VENDOR="$REPO_ROOT/third_party"
MUSL_LIBDIR="$MUSL_ROOT/usr/lib/x86_64-linux-musl"

rm -rf "$MUSL_ROOT" "$TCC_ROOT" "$TCC_SELF" "$WORK_ROOT"
mkdir -p "$MUSL_ROOT" "$WORK_ROOT"

# --------------------------------------------------------------------------
# 1) musl libc — built from vendored source and installed into a Debian
#    multiarch layout so the Makefile's $(MUSL_LIBDIR)/musl-gcc.specs rule
#    (which rewrites the /usr/include|/usr/lib|/lib multiarch paths to point
#    into MUSL_ROOT) works without any change.
# --------------------------------------------------------------------------
tar -C "$WORK_ROOT" -xf "$VENDOR/musl/musl-$MUSL_VERSION.tar.gz"
cd "$WORK_ROOT/musl-$MUSL_VERSION"
./configure --prefix=/usr \
    --includedir=/usr/include/x86_64-linux-musl \
    --libdir=/usr/lib/x86_64-linux-musl \
    --syslibdir=/lib/x86_64-linux-musl \
    CC=gcc
make -j"$JOBS"
make lib/musl-gcc.specs           # gcc wrapper/specs are off by default; build explicitly
make install DESTDIR="$MUSL_ROOT"
cp lib/musl-gcc.specs "$MUSL_LIBDIR/musl-gcc.specs"

# --------------------------------------------------------------------------
# 2) TinyCC — built musl-static from vendored source.
# --------------------------------------------------------------------------
tar -C "$WORK_ROOT" -xf "$VENDOR/tinycc/$TCC_TARBALL"
mv "$WORK_ROOT/tinycc" "$TCC_ROOT"
make -C "$REPO_ROOT" MUSL_ROOT="$MUSL_ROOT" build/musl-gcc.specs
cd "$TCC_ROOT"
CC="gcc -specs=$REPO_ROOT/build/musl-gcc.specs" ./configure \
    --prefix=/ --tccdir=/lib/tcc --includedir=/include \
    --sysincludepaths=/include --libpaths=/lib --crtprefix=/lib \
    --elfinterp= --cc="gcc -specs=$REPO_ROOT/build/musl-gcc.specs" \
    --extra-cflags="-Os -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -s" \
    --extra-ldflags="-static -no-pie" --cpu=x86_64 --targetos=Linux
sed -i 's|$S$(CC) -DC2STR $< -o $@|$S$(CC) -DC2STR $< -o $@ $(LDFLAGS)|' Makefile
# TinyOS has no dynamic linker, so default to static linking — lets a bare
# `tcc a.c -o a.bin` produce a runnable static ELF without an explicit -static.
sed -i 's/^    s->unwind_tables = 1;$/    s->unwind_tables = 1;\n    s->static_link = 1;/' libtcc.c
make -j"$JOBS" tcc \
    CC="gcc -specs=$REPO_ROOT/build/musl-gcc.specs" \
    CFLAGS="-Os -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -s" \
    LDFLAGS="-static -no-pie"
make -C lib ../libtcc1.a \
    XFLAGS="-B$TCC_ROOT -I$MUSL_ROOT/usr/include/x86_64-linux-musl -I$TCC_ROOT -I$TCC_ROOT/include"

# --------------------------------------------------------------------------
# 3) self-host root — tcc sources + musl runtime + preprocessed self-build
#    inputs (TCCSELF.I / BUILD.RSP) used by the on-device TCC self-host test.
# --------------------------------------------------------------------------
mkdir -p "$TCC_SELF/src" "$TCC_SELF/include" "$TCC_SELF/lib"
for source in \
    config.h dwarf.h elf.h i386-asm.c i386-asm.h i386-tok.h \
    libtcc.c libtcc.h stab.def stab.h tcc.c tcc.h tccasm.c tccdbg.c \
    tccdefs_.h tccelf.c tccgen.c tccpp.c tccrun.c tcctok.h tcctools.c \
    x86_64-asm.h x86_64-gen.c x86_64-link.c; do
    cp "$TCC_ROOT/$source" "$TCC_SELF/src/$source"
done
cp -R "$MUSL_ROOT/usr/include/x86_64-linux-musl/." "$TCC_SELF/include/"
for runtime in crt1.o crti.o crtn.o libc.a libdl.a libm.a libpthread.a; do
    cp "$MUSL_LIBDIR/$runtime" "$TCC_SELF/lib/$runtime"
done
cp "$TCC_ROOT/libtcc1.a" "$TCC_SELF/lib/libtcc1.a"

"$TCC_ROOT/tcc" -E \
    -I"$TCC_SELF/include" -I"$TCC_SELF/src" \
    -DONE_SOURCE=1 -DTCC_TARGET_X86_64 \
    "$TCC_SELF/src/tcc.c" > "$TCC_SELF/TCCSELF.I"
cat > "$TCC_SELF/BUILD.RSP" <<'EOF'
-x
c
/TCCSELF.I
-x
n
-static
-nostdinc
-nostdlib
/CRT1.O
/CRTI.O
/LIBC.A
/LIBTCC1.A
/LIBC.A
/CRTN.O
-o
/TCC2.BIN
EOF

echo "TinyOS toolchain staged from vendored source (musl $MUSL_VERSION, tcc $TCC_REV):"
echo "  MUSL_ROOT=$MUSL_ROOT"
echo "  TCC_BIN=$TCC_ROOT/tcc"
echo "  TCC_SELF=$TCC_SELF"
