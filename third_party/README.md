## uACPI

`uacpi/` is uACPI 6.0.0 at commit `9c9b26d6291a1cdd9014cc5bb6b03e596697cbfd`,
vendored unchanged under its MIT license. TinyOS uses it to execute platform
ACPI sleep methods such as `_PTS` and `_WAK`.

The uACPI objects are compiled with `-fPIE` under TinyOS's forced hidden
declaration context. Keep both settings: ordinary `-fPIC` creates GOT references
that GNU's PE linker mishandles, while `-fno-pic` leaves preferred-base pointers
that fail when physical firmware relocates `BOOTX64.EFI`.

# third_party — vendored build materials

TinyOS's **target toolchain** is built from source vendored here, so all three
images (QEMU, Hyper-V, and the T440s USB stick) build on any machine that has a
standard host toolchain (`gcc`, `make`, binutils, Python) — **no network access
and no distro packages are required**. `tools/setup_tcc.sh` (run by
`make setup-tcc`) unpacks and builds these into `/tmp`
(`MUSL_ROOT`, `TCC_BIN`, `TCC_SELF`).

## Contents

| Path | Upstream | Version / revision | SHA-256 |
|------|----------|--------------------|---------|
| `musl/musl-1.2.4.tar.gz` | <https://musl.libc.org/releases/> | 1.2.4 (official release) | `7a35eae33d5372a7c0da1188de798726f68825513b7ae3ebe97aaaa52114f039` |
| `tinycc/tinycc-38059770.tar.gz` | <https://repo.or.cz/tinycc.git> (`mob`) | `380597704ee9784442ef3c7ef06e105258a11c5d` | `441f5b04bf2d4382c129c6185bcc0447e8bd2f2ee571aea753c52ca83af6f408` |

- **musl** — the official 1.2.4 source tarball (the SHA-256 above matches
  musl.libc.org). `setup_tcc.sh` builds it with the host `gcc` and installs it
  into a Debian multiarch layout (`usr/lib/x86_64-linux-musl/…`, plus a
  generated `musl-gcc.specs`) so the Makefile consumes it unchanged. MIT.
- **tinycc** — a pristine `git archive` of the pinned `mob` revision (repo.or.cz
  publishes no matching release tarball, and refuses shallow fetches of an
  unadvertised commit, which is why the source is vendored rather than fetched).
  `setup_tcc.sh` applies two small in-tree patches at build time (static linking
  by default; `LDFLAGS` on the `C2STR` host tool). LGPL 2.1.

## Updating

Recreate a tarball and refresh its row above:

```sh
# musl — official release tarball
curl -LO https://musl.libc.org/releases/musl-<ver>.tar.gz

# tinycc — from a checkout of <rev> (full clone, then archive the rev)
git clone https://repo.or.cz/tinycc.git && git -C tinycc archive \
    --format=tar.gz --prefix=tinycc/ <rev> > tinycc-<short>.tar.gz

sha256sum musl-<ver>.tar.gz tinycc-<short>.tar.gz
```
