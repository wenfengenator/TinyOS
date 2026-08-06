#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Build a FAT16 disk image from a source directory (one level of subdirs).

    mkfatfs.py <out.img> <srcdir> [size_kb] [NAME=path ...]

Produces a 4 MB FAT16 image tinyos can mount.  (Images made by mkfs.vfat -F16
also work with tinyos's reader.)
"""
import sys, os, struct

SECSZ = 512
SPC = 1                      # sectors per cluster
RESERVED = 1
NFATS = 2
ROOT_ENTRIES = 512

# Image geometry — recomputed by configure() from a size in KB (default 4 MB).
TOTAL_SECTORS = 8192         # 4 MB
FAT_SECTORS = 32             # 8192 FAT16 entries
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECSZ            # 32
DATA_START = RESERVED + NFATS * FAT_SECTORS + ROOT_SECTORS   # sector of cluster 2


def configure(size_kb):
    """Recompute the image geometry for a disk of size_kb kilobytes."""
    global TOTAL_SECTORS, FAT_SECTORS, ROOT_SECTORS, DATA_START
    TOTAL_SECTORS = size_kb * 1024 // SECSZ
    FAT_SECTORS   = (TOTAL_SECTORS * 2 + SECSZ - 1) // SECSZ     # 2 bytes / entry
    ROOT_SECTORS  = ROOT_ENTRIES * 32 // SECSZ
    DATA_START    = RESERVED + NFATS * FAT_SECTORS + ROOT_SECTORS


def short_raw(name83):
    if name83 == ".":
        return b".          "
    if name83 == "..":
        return b"..         "
    base, _, ext = name83.rpartition(".")
    if not base:
        base, ext = ext, ""
    return (base.upper()[:8].ljust(8) + ext.upper()[:3].ljust(3)).encode()


def dirent_raw(nm, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = nm
    e[11] = attr
    struct.pack_into("<H", e, 26, cluster)      # first cluster (low word)
    struct.pack_into("<I", e, 28, size)         # file size
    return bytes(e)


def dirent(name83, attr, cluster, size):
    return dirent_raw(short_raw(name83), attr, cluster, size)


def fits_83(name):
    base, dot, ext = name.rpartition(".")
    if not dot:
        base, ext = name, ""
    valid = "$%'-_@~`!(){}^#&"
    return (0 < len(base) <= 8 and len(ext) <= 3 and
            all(c.isalnum() or c in valid for c in base + ext))


def alias_raw(name, used):
    base, dot, ext = name.rpartition(".")
    if not dot:
        base, ext = name, ""
    clean = "".join(c if c.isalnum() else "_" for c in base).upper() or "_"
    ext = "".join(c if c.isalnum() else "_" for c in ext).upper()[:3]
    for number in range(1, 100000):
        suffix = "~" + str(number)
        alias = clean[:8 - len(suffix)] + suffix
        raw = (alias.ljust(8) + ext.ljust(3)).encode()
        if raw not in used:
            return raw
    raise RuntimeError("cannot create FAT alias for " + name)


def lfn_checksum(raw):
    value = 0
    for byte in raw:
        value = (((value & 1) << 7) + (value >> 1) + byte) & 0xFF
    return value


def named_dirents(name, attr, cluster, size, used):
    if fits_83(name):
        raw = short_raw(name)
        if raw not in used:
            used.add(raw)
            return [dirent_raw(raw, attr, cluster, size)]
    raw = alias_raw(name, used)
    used.add(raw)
    checksum = lfn_checksum(raw)
    positions = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
    count = (len(name) + 12) // 13
    entries = []
    for order in range(count, 0, -1):
        entry = bytearray(32)
        entry[0] = order | (0x40 if order == count else 0)
        entry[11] = 0x0F
        entry[13] = checksum
        base = (order - 1) * 13
        for index, pos in enumerate(positions):
            offset = base + index
            char = ord(name[offset]) if offset < len(name) else (0 if offset == len(name) else 0xFFFF)
            struct.pack_into("<H", entry, pos, char)
        entries.append(bytes(entry))
    entries.append(dirent_raw(raw, attr, cluster, size))
    return entries


def entry_slots(name):
    return 1 if fits_83(name) else 1 + (len(name) + 12) // 13


class Fat:
    def __init__(self):
        self.fat = [0] * (FAT_SECTORS * SECSZ // 2)
        self.fat[0] = 0xFFF8; self.fat[1] = 0xFFFF
        self.next = 2
        self.clusters = {}                # cluster -> 512-byte sector bytes
    def alloc_chain(self, data):
        data = data + b"\0" * ((-len(data)) % SECSZ)
        n = max(1, len(data) // SECSZ)
        first = self.next
        for i in range(n):
            c = self.next; self.next += 1
            self.clusters[c] = data[i*SECSZ:(i+1)*SECSZ]
            self.fat[c] = 0xFFFF if i == n - 1 else c + 1
        return first, len(data)

    def reserve(self, nclus):
        """Reserve a chain of nclus clusters (filled later by write_chain)."""
        first = self.next
        for i in range(nclus):
            c = self.next; self.next += 1
            self.clusters[c] = b"\0" * SECSZ
            self.fat[c] = 0xFFFF if i == nclus - 1 else c + 1
        return first

    def write_chain(self, first, data):
        """Write data into an already-reserved cluster chain."""
        data = data + b"\0" * ((-len(data)) % SECSZ)
        c = first
        for i in range(0, len(data), SECSZ):
            self.clusters[c] = data[i:i+SECSZ]
            nxt = self.fat[c]
            if nxt >= 0xFFF8:
                break
            c = nxt


def build_dir(fat, path, parent_cluster):
    """Recursively lay out a directory (any nesting) and return its first cluster.
    ".." points at parent_cluster (0 == root), so nested paths like
    /include/bits/alltypes.h resolve correctly."""
    items   = sorted(os.listdir(path))
    files   = [f for f in items if os.path.isfile(os.path.join(path, f))]
    subdirs = [d for d in items if os.path.isdir(os.path.join(path, d))]
    nent    = 2 + sum(entry_slots(name) for name in files + subdirs)
    nclus   = max(1, (nent * 32 + SECSZ - 1) // SECSZ)
    self_cluster = fat.reserve(nclus)                        # reserve before children
    ents = [dirent(".", 0x10, self_cluster, 0),
            dirent("..", 0x10, parent_cluster, 0)]
    used = {short_raw("."), short_raw("..")}
    for f in files:
        data = open(os.path.join(path, f), "rb").read()
        c, _ = fat.alloc_chain(data)
        ents.extend(named_dirents(f, 0x20, c, len(data), used))
    for d in subdirs:
        c = build_dir(fat, os.path.join(path, d), self_cluster)
        ents.extend(named_dirents(d, 0x10, c, 0, used))
    fat.write_chain(self_cluster, b"".join(ents))
    return self_cluster


def build(srcdir, extra_files=()):
    fat = Fat()
    root = b""
    used = set()

    # subdirectories (recursive: one FAT entry per top-level dir, nested below)
    for entry in sorted(os.listdir(srcdir)):
        p = os.path.join(srcdir, entry)
        if os.path.isdir(p):
            c = build_dir(fat, p, 0)                  # parent == root (cluster 0)
            root += b"".join(named_dirents(entry, 0x10, c, 0, used))

    # root files
    for entry in sorted(os.listdir(srcdir)):
        p = os.path.join(srcdir, entry)
        if os.path.isfile(p):
            data = open(p, "rb").read()
            c, sz = fat.alloc_chain(data)
            root += b"".join(named_dirents(entry, 0x20, c, len(data), used))

    for name, path in extra_files:
        data = open(path, "rb").read()
        c, sz = fat.alloc_chain(data)
        root += b"".join(named_dirents(name, 0x20, c, len(data), used))

    # ---- assemble image ----
    img = bytearray(TOTAL_SECTORS * SECSZ)

    # boot sector / BPB
    bs = bytearray(SECSZ)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"TINYOS  "
    struct.pack_into("<H", bs, 11, SECSZ)
    bs[13] = SPC
    struct.pack_into("<H", bs, 14, RESERVED)
    bs[16] = NFATS
    struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
    struct.pack_into("<H", bs, 19, TOTAL_SECTORS)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 22, FAT_SECTORS)
    struct.pack_into("<H", bs, 24, 32)      # sectors/track
    struct.pack_into("<H", bs, 26, 2)       # heads
    bs[36] = 0x80; bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0x12345678)
    bs[43:54] = b"TINYOS FS  "
    bs[54:62] = b"FAT16   "
    bs[510] = 0x55; bs[511] = 0xAA
    img[0:SECSZ] = bs

    # FATs
    fatbytes = b"".join(struct.pack("<H", e & 0xFFFF) for e in fat.fat)
    for n in range(NFATS):
        off = (RESERVED + n * FAT_SECTORS) * SECSZ
        img[off:off + len(fatbytes)] = fatbytes

    # root directory
    off = (RESERVED + NFATS * FAT_SECTORS) * SECSZ
    img[off:off + len(root)] = root

    # data clusters
    for c, data in fat.clusters.items():
        sec = DATA_START + (c - 2) * SPC
        img[sec * SECSZ: sec * SECSZ + SECSZ] = data

    return bytes(img)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    size_kb = int(sys.argv[3]) if len(sys.argv) >= 4 else 4096
    extra_files = []
    for spec in sys.argv[4:]:
        if "=" not in spec:
            sys.exit("invalid extra file (expected NAME=path): " + spec)
        extra_files.append(spec.split("=", 1))
    configure(size_kb)
    image = build(sys.argv[2], extra_files)
    temp = sys.argv[1] + ".tmp"
    open(temp, "wb").write(image)
    os.replace(temp, sys.argv[1])
    print("FAT16 image: %s (%d KB) from %s/" % (sys.argv[1], size_kb, sys.argv[2]))


if __name__ == "__main__":
    main()
