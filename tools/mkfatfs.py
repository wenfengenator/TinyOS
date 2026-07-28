#!/usr/bin/env python3
"""Build a FAT16 disk image from a source directory (one level of subdirs).

  mkfatfs.py <out.img> <srcdir>

Produces a 4 MB FAT16 image tinyos can mount.  (Images made by mkfs.vfat -F16
also work with tinyos's reader.)
"""
import sys, os, struct

SECSZ = 512
SPC = 1                      # sectors per cluster
RESERVED = 1
NFATS = 2
ROOT_ENTRIES = 512
TOTAL_SECTORS = 8192         # 4 MB
FAT_SECTORS = 32             # 8192 FAT16 entries
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECSZ            # 32
DATA_START = RESERVED + NFATS * FAT_SECTORS + ROOT_SECTORS   # sector of cluster 2


def dirent(name83, attr, cluster, size):
    base, _, ext = name83.partition(".")
    nm = (base.upper()[:8].ljust(8) + ext.upper()[:3].ljust(3)).encode()
    e = bytearray(32)
    e[0:11] = nm
    e[11] = attr
    struct.pack_into("<H", e, 26, cluster)      # first cluster (low word)
    struct.pack_into("<I", e, 28, size)         # file size
    return bytes(e)


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


def build(srcdir):
    fat = Fat()
    root = b""

    # subdirectories (one level)
    for entry in sorted(os.listdir(srcdir)):
        p = os.path.join(srcdir, entry)
        if os.path.isdir(p):
            # build the directory's cluster: '.', '..', then files
            files = []
            for f in sorted(os.listdir(p)):
                fp = os.path.join(p, f)
                if os.path.isfile(fp):
                    data = open(fp, "rb").read()
                    c, sz = fat.alloc_chain(data)
                    files.append(dirent(f, 0x20, c, len(data)))
            dcluster = fat.next
            dirdata = dirent(".", 0x10, dcluster, 0) + dirent("..", 0x10, 0, 0) + b"".join(files)
            c2, _ = fat.alloc_chain(dirdata)     # dcluster == c2
            root += dirent(entry, 0x10, c2, 0)

    # root files
    for entry in sorted(os.listdir(srcdir)):
        p = os.path.join(srcdir, entry)
        if os.path.isfile(p):
            data = open(p, "rb").read()
            c, sz = fat.alloc_chain(data)
            root += dirent(entry, 0x20, c, len(data))

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
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    open(sys.argv[1], "wb").write(build(sys.argv[2]))
    print("FAT16 image: %s (4 MB) from %s/" % (sys.argv[1], sys.argv[2]))


if __name__ == "__main__":
    main()
