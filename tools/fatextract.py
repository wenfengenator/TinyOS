#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Extract one 8.3 file from the root directory of a FAT16 image."""
import sys, struct

def extract(img_path, name83, out_path):
    d = open(img_path, 'rb').read()
    bps = struct.unpack_from('<H', d, 11)[0]
    spc = d[13]
    resv = struct.unpack_from('<H', d, 14)[0]
    nfat = d[16]
    root_entries = struct.unpack_from('<H', d, 17)[0]
    spf = struct.unpack_from('<H', d, 22)[0]
    fat_start = resv * bps
    root_start = fat_start + nfat * spf * bps
    root_sectors = (root_entries * 32 + bps - 1) // bps
    data_start = root_start + root_sectors * bps
    fat = d[fat_start:fat_start + spf * bps]
    # find the dir entry
    for off in range(root_start, data_start, 32):
        ent = d[off:off + 32]
        if ent[0] in (0x00, 0xE5) or ent[11] == 0x0F:
            continue
        if ent[0:11] == name83.encode():
            first = struct.unpack_from('<H', ent, 26)[0]
            size = struct.unpack_from('<I', ent, 28)[0]
            out = bytearray()
            cl = first
            while 2 <= cl < 0xFFF8 and len(out) < size:
                sec = data_start + (cl - 2) * spc * bps
                out += d[sec:sec + spc * bps]
                cl = struct.unpack_from('<H', fat, cl * 2)[0]
            open(out_path, 'wb').write(bytes(out[:size]))
            print(f"extracted {name83!r} -> {out_path} ({size} bytes)")
            return 0
    print(f"error: {name83!r} not found in {img_path}", file=sys.stderr)
    return 1

if __name__ == '__main__':
    sys.exit(extract(sys.argv[1], sys.argv[2], sys.argv[3]))
