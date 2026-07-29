#!/usr/bin/env python3
"""Build a GPT disk image with a FAT32 ESP and a FAT16 TinyOS data partition.

    mkgptesp.py <out.img> <esp_dir> <data.img>

Populates the ESP with the contents of <esp_dir> (recursively) and wraps a
FAT32 volume in a GPT-partitioned disk that UEFI firmware (OVMF, and Hyper-V
Gen 2) will boot from the removable-media path /EFI/BOOT/BOOTX64.EFI.

No mkfs.vfat / mtools required — the FAT32 volume and the GPT are written by
hand.  Convert the result to VHDX with `qemu-img convert -O vhdx`.
"""
import sys, os, struct, binascii, uuid

SECT = 512
SPC = 1                              # sectors per cluster (512-byte clusters)
RESERVED = 32                        # FAT32 reserved sectors (boot + fsinfo + backups)
NFATS = 2
PART_SECTORS = 131072                # 64 MiB ESP  (>> 65525 clusters => real FAT32)
PART_START_LBA = 2048                # 1 MiB alignment
DATA_START_LBA = PART_START_LBA + PART_SECTORS

ESP_TYPE_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
DATA_TYPE_GUID = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")


# ---------------- FAT32 volume ----------------
class Fat32:
    def __init__(self, part_sectors):
        self.part_sectors = part_sectors
        tmp1 = part_sectors - RESERVED
        tmp2 = (256 * SPC + NFATS) // 2
        self.fatsz = (tmp1 + tmp2 - 1) // tmp2               # sectors per FAT
        self.data_start = RESERVED + NFATS * self.fatsz
        self.nclusters = (part_sectors - self.data_start) // SPC
        self.csize = SPC * SECT
        self.fat = [0] * (self.nclusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.data = {}                                       # cluster -> bytes
        self.next = 3                                        # cluster 2 reserved for root

    def _alloc(self):
        c = self.next; self.next += 1
        if c >= self.nclusters + 2:
            raise SystemExit("mkgptesp: ESP too small for its contents")
        self.fat[c] = 0x0FFFFFFF
        return c

    def store(self, first, data):
        """Write data into a cluster chain beginning at `first` (already reserved)."""
        n = max(1, (len(data) + self.csize - 1) // self.csize)
        data = data.ljust(n * self.csize, b"\0")
        c = first
        for i in range(n):
            self.data[c] = data[i * self.csize:(i + 1) * self.csize]
            if i == n - 1:
                self.fat[c] = 0x0FFFFFFF
            else:
                nxt = self._alloc()
                self.fat[c] = nxt
                c = nxt

    def alloc_file(self, data):
        if len(data) == 0:
            return 0
        first = self._alloc()
        self.store(first, data)
        return first

    @staticmethod
    def dirent(name, attr, cluster, size):
        if name in (".", ".."):
            nm = (name.ljust(8) + "".ljust(3)).encode()
        else:
            base, _, ext = name.partition(".")
            nm = (base.upper()[:8].ljust(8) + ext.upper()[:3].ljust(3)).encode()
        e = bytearray(32)
        e[0:11] = nm
        e[11] = attr
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)   # first cluster high
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)           # first cluster low
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def add_dir(self, path, self_cluster, parent_cluster, is_root):
        entries = []
        if not is_root:
            entries.append(self.dirent(".", 0x10, self_cluster, 0))
            entries.append(self.dirent("..", 0x10, parent_cluster, 0))
        for name in sorted(os.listdir(path)):
            p = os.path.join(path, name)
            if os.path.isdir(p):
                child = self._alloc()
                self.add_dir(p, child, self_cluster, False)
                entries.append(self.dirent(name, 0x10, child, 0))
            else:
                d = open(p, "rb").read()
                entries.append(self.dirent(name, 0x20, self.alloc_file(d), len(d)))
        self.store(self_cluster, b"".join(entries))

    def image(self):
        # ---- boot sector (BPB) ----
        bs = bytearray(SECT)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<H", bs, 11, SECT)
        bs[13] = SPC
        struct.pack_into("<H", bs, 14, RESERVED)
        bs[16] = NFATS
        struct.pack_into("<H", bs, 17, 0)                    # root entries (FAT32=0)
        struct.pack_into("<H", bs, 19, 0)                    # total16
        bs[21] = 0xF8
        struct.pack_into("<H", bs, 22, 0)                    # fatsz16 (FAT32=0)
        struct.pack_into("<H", bs, 24, 0x20)                 # sectors/track
        struct.pack_into("<H", bs, 26, 0x40)                 # heads
        struct.pack_into("<I", bs, 28, PART_START_LBA)       # hidden sectors
        struct.pack_into("<I", bs, 32, self.part_sectors)    # total32
        struct.pack_into("<I", bs, 36, self.fatsz)           # fatsz32
        struct.pack_into("<H", bs, 40, 0)                    # ext flags
        struct.pack_into("<H", bs, 42, 0)                    # fs version
        struct.pack_into("<I", bs, 44, 2)                    # root cluster
        struct.pack_into("<H", bs, 48, 1)                    # fsinfo sector
        struct.pack_into("<H", bs, 50, 6)                    # backup boot sector
        bs[64] = 0x80
        bs[66] = 0x29                                        # extended boot sig
        struct.pack_into("<I", bs, 67, 0x54494E59)           # volume id
        bs[71:82] = b"TINYOS ESP "
        bs[82:90] = b"FAT32   "
        struct.pack_into("<H", bs, 510, 0xAA55)

        # ---- FSInfo ----
        fsi = bytearray(SECT)
        struct.pack_into("<I", fsi, 0, 0x41615252)
        struct.pack_into("<I", fsi, 484, 0x61417272)
        struct.pack_into("<I", fsi, 488, 0xFFFFFFFF)
        struct.pack_into("<I", fsi, 492, 0xFFFFFFFF)
        struct.pack_into("<H", fsi, 510, 0xAA55)

        img = bytearray(self.part_sectors * SECT)
        img[0:SECT] = bs
        img[SECT:2 * SECT] = fsi
        img[6 * SECT:7 * SECT] = bs                          # backup boot
        img[7 * SECT:8 * SECT] = fsi

        # ---- FATs ----
        fatbytes = bytearray(self.fatsz * SECT)
        for i, v in enumerate(self.fat):
            struct.pack_into("<I", fatbytes, i * 4, v & 0x0FFFFFFF)
        for f in range(NFATS):
            off = (RESERVED + f * self.fatsz) * SECT
            img[off:off + len(fatbytes)] = fatbytes

        # ---- data region ----
        for c, d in self.data.items():
            off = (self.data_start + (c - 2) * SPC) * SECT
            img[off:off + len(d)] = d
        return bytes(img)


def build_fat(esp_dir):
    fs = Fat32(PART_SECTORS)
    fs.add_dir(esp_dir, 2, 0, True)                          # root = cluster 2
    return fs.image()


# ---------------- GPT wrapper ----------------
def partition_entry(type_guid, first_lba, last_lba, name):
    entry = bytearray(128)
    entry[0:16] = type_guid.bytes_le
    entry[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<Q", entry, 32, first_lba)
    struct.pack_into("<Q", entry, 40, last_lba)
    encoded_name = name.encode("utf-16-le")
    entry[56:56 + len(encoded_name)] = encoded_name
    return entry


def gpt_disk(part_img, data_img):
    part_sectors = len(part_img) // SECT
    part_end = PART_START_LBA + part_sectors - 1
    if len(data_img) % SECT:
        raise SystemExit("mkgptesp: data image must be sector-aligned")
    data_sectors = len(data_img) // SECT
    data_end = DATA_START_LBA + data_sectors - 1
    disk_sectors = data_end + 1 + 33                         # +backup array(32)+header(1)
    backup_hdr_lba = disk_sectors - 1
    backup_arr_lba = disk_sectors - 33
    disk = bytearray(disk_sectors * SECT)

    # place the partition
    disk[PART_START_LBA * SECT: PART_START_LBA * SECT + len(part_img)] = part_img
    disk[DATA_START_LBA * SECT: DATA_START_LBA * SECT + len(data_img)] = data_img

    # protective MBR
    mbr = bytearray(SECT)
    mbr[446 + 0] = 0x00
    mbr[446 + 1:446 + 4] = b"\x00\x02\x00"
    mbr[446 + 4] = 0xEE
    mbr[446 + 5:446 + 8] = b"\xFF\xFF\xFF"
    struct.pack_into("<I", mbr, 446 + 8, 1)
    struct.pack_into("<I", mbr, 446 + 12, min(disk_sectors - 1, 0xFFFFFFFF))
    struct.pack_into("<H", mbr, 510, 0xAA55)
    disk[0:SECT] = mbr

    # partition entry array
    arr = bytearray(128 * 128)
    arr[0:128] = partition_entry(ESP_TYPE_GUID, PART_START_LBA, part_end, "EFI System Partition")
    arr[128:256] = partition_entry(DATA_TYPE_GUID, DATA_START_LBA, data_end, "TinyOS Data")
    arr_crc = binascii.crc32(arr) & 0xFFFFFFFF

    disk_guid = uuid.uuid4().bytes_le

    def header(current_lba, other_lba, arr_lba):
        h = bytearray(SECT)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)
        struct.pack_into("<I", h, 12, 92)
        struct.pack_into("<Q", h, 24, current_lba)
        struct.pack_into("<Q", h, 32, other_lba)
        struct.pack_into("<Q", h, 40, 34)                   # first usable
        struct.pack_into("<Q", h, 48, disk_sectors - 34)    # last usable
        h[56:72] = disk_guid
        struct.pack_into("<Q", h, 72, arr_lba)
        struct.pack_into("<I", h, 80, 128)                  # num entries
        struct.pack_into("<I", h, 84, 128)                  # entry size
        struct.pack_into("<I", h, 88, arr_crc)
        struct.pack_into("<I", h, 16, binascii.crc32(h[0:92]) & 0xFFFFFFFF)
        return h

    disk[1 * SECT:2 * SECT] = header(1, backup_hdr_lba, 2)
    disk[2 * SECT:2 * SECT + len(arr)] = arr
    disk[backup_arr_lba * SECT:backup_arr_lba * SECT + len(arr)] = arr
    disk[backup_hdr_lba * SECT:(backup_hdr_lba + 1) * SECT] = header(backup_hdr_lba, 1, backup_arr_lba)
    return bytes(disk)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: mkgptesp.py <out.img> <esp_dir> <data.img>")
    out, esp, data_path = sys.argv[1], sys.argv[2], sys.argv[3]
    disk = gpt_disk(build_fat(esp), open(data_path, "rb").read())
    open(out, "wb").write(disk)
    print("GPT ESP+data image: %s (%d MiB)" % (out, len(disk) // (1024 * 1024)))


if __name__ == "__main__":
    main()
