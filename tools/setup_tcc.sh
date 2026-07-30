#!/bin/sh
set -eu

TCC_REV=${TCC_REV:-380597704ee9784442ef3c7ef06e105258a11c5d}
MUSL_VERSION=${MUSL_VERSION:-1.2.4-2}
MUSL_ROOT=${MUSL_ROOT:-/tmp/tinyos-musl}
TCC_ROOT=${TCC_ROOT:-/tmp/tinycc}
TCC_SELF=${TCC_SELF:-/tmp/tcc-selfroot}
WORK_ROOT=${WORK_ROOT:-/tmp/tinyos-toolchain}
REPO_ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

rm -rf "$MUSL_ROOT" "$TCC_ROOT" "$TCC_SELF" "$WORK_ROOT"
mkdir -p "$MUSL_ROOT" "$WORK_ROOT/packages"

cd "$WORK_ROOT/packages"
apt-get download \
    "musl=$MUSL_VERSION" \
    "musl-dev=$MUSL_VERSION" \
    "musl-tools=$MUSL_VERSION"
for package in ./*.deb; do
    dpkg-deb -x "$package" "$MUSL_ROOT"
done

git init -q "$TCC_ROOT"
git -C "$TCC_ROOT" remote add origin https://repo.or.cz/tinycc.git
git -C "$TCC_ROOT" fetch -q --depth 1 origin "$TCC_REV"
git -C "$TCC_ROOT" checkout -q --detach FETCH_HEAD

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
make -j2 tcc \
    CC="gcc -specs=$REPO_ROOT/build/musl-gcc.specs" \
    CFLAGS="-Os -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -s" \
    LDFLAGS="-static -no-pie"
make -C lib ../libtcc1.a \
    XFLAGS="-B$TCC_ROOT -I$MUSL_ROOT/usr/include/x86_64-linux-musl -I$TCC_ROOT -I$TCC_ROOT/include"

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
    cp "$MUSL_ROOT/usr/lib/x86_64-linux-musl/$runtime" "$TCC_SELF/lib/$runtime"
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

echo "TinyCC $TCC_REV staged in $TCC_ROOT and $TCC_SELF"