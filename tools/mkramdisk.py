#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Pack tinyos ramdisk programs into a simple archive (magic 'TRD').

Layout:  'TRD\\0' | count(u32) | count*{ name[16], off(u32), size(u32) } | blobs
Offsets are relative to the start of the archive.

Usage:  mkramdisk.py out.img  name1 file1  name2 file2  ...
"""
import sys, struct

def main():
    out = sys.argv[1]
    args = sys.argv[2:]
    items = [(args[i], args[i + 1]) for i in range(0, len(args), 2)]
    hdr = 8 + 24 * len(items)
    blobs = b""
    ents = []
    for name, path in items:
        data = open(path, "rb").read()
        ents.append((name, hdr + len(blobs), len(data)))
        blobs += data
    buf = b"TRD\0" + struct.pack("<I", len(items))
    for name, off, size in ents:
        nb = name.encode()[:15]
        buf += nb + b"\0" * (16 - len(nb)) + struct.pack("<II", off, size)
    buf += blobs
    open(out, "wb").write(buf)
    print("ramdisk: %d programs, %d bytes -> %s" % (len(items), len(buf), out))

if __name__ == "__main__":
    main()
