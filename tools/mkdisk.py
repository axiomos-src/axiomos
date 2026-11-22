#!/usr/bin/env python3
"""Build the AxiomOS GPT/FAT32 disk image without external tools.

The image is produced entirely in Python: a protective MBR, a GPT header with
one ESP partition, and a FAT32 filesystem inside that partition.  Doing it by
hand (instead of calling mkfs.vfat) keeps the build reproducible, avoids
requiring root privileges, and makes it impossible for the build to touch a
real block device.

Layout of the 128 MiB image (512 byte sectors):

    0                protective MBR
    1                GPT header
    2 .. 33          GPT partition entry array (128 entries)
    2048 ..          ESP partition, FAT32 with 4 KiB clusters

Usage:
    python3 tools/mkdisk.py --esp build/esp --out build/axiomos.img
"""

import argparse
import datetime
import os
import struct
import sys
import zlib

SECTOR = 512
IMAGE_MB = int(os.environ.get("AXIOM_IMAGE_MB", "128"))
ESP_MB = int(os.environ.get("AXIOM_ESP_MB", "64"))
IMAGE_SECTORS = IMAGE_MB * 1024 * 1024 // SECTOR
ESP_START_LBA = 2048
ESP_SECTORS = ESP_MB * 1024 * 1024 // SECTOR
SECTORS_PER_CLUSTER = 8                  # 4 KiB clusters
RESERVED_SECTORS = 32
NUM_FATS = 2
ROOT_CLUSTER = 2

ESP_TYPE_GUID = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
BASIC_DATA_GUID = bytes.fromhex("a2a0d0eb05b44c33996e91e1a3b3dd7902")

ATTR_READ_ONLY = 0x01
ATTR_HIDDEN = 0x02
ATTR_SYSTEM = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


class Fat32Writer:
    """A minimal FAT32 formatter and file writer."""

    def __init__(self, image: bytearray, start_lba: int, total_sectors: int):
        self.image = image
        self.start = start_lba
        self.total = total_sectors
        self.clusters = total_sectors // SECTORS_PER_CLUSTER
        # One FAT entry per cluster; FAT size in sectors must be a multiple of
        # the sector size and large enough for all entries plus the two
        # reserved entries.
        entry_bytes = (self.clusters + 2) * 4
        self.fat_sectors = (entry_bytes + SECTOR - 1) // SECTOR
        # Round up so that the second FAT starts on a sector boundary.
        self.data_start = RESERVED_SECTORS + NUM_FATS * self.fat_sectors
        self.next_free = ROOT_CLUSTER + 1
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF

    # -- geometry helpers ------------------------------------------------
    def sector_offset(self, lba: int) -> int:
        return (self.start + lba) * SECTOR

    def cluster_lba(self, cluster: int) -> int:
        return self.data_start + (cluster - 2) * SECTORS_PER_CLUSTER

    def allocate(self, count: int) -> int:
        if self.next_free + count > self.clusters + 2:
            raise RuntimeError("ESP image is full")
        first = self.next_free
        for i in range(count):
            self.fat[first + i] = first + i + 1 if i + 1 < count else 0x0FFFFFFF
        self.next_free += count
        return first

    def write_cluster_data(self, cluster: int, data: bytes):
        offset = self.sector_offset(self.cluster_lba(cluster))
        length = SECTORS_PER_CLUSTER * SECTOR
        chunk = data[:length].ljust(length, b"\x00")
        self.image[offset:offset + length] = chunk

    # -- filesystem construction -----------------------------------------
    def format(self, volume_label="AXIOMOS"):
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x58\x90"
        boot[3:11] = b"AXIOMOS "
        struct.pack_into("<H", boot, 11, SECTOR)
        boot[13] = SECTORS_PER_CLUSTER
        struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
        boot[16] = NUM_FATS
        boot[21] = 0xF8                     # media descriptor: fixed disk
        struct.pack_into("<H", boot, 24, 63)
        struct.pack_into("<H", boot, 26, 255)
        struct.pack_into("<I", boot, 28, self.start)
        struct.pack_into("<I", boot, 32, self.total)
        struct.pack_into("<I", boot, 36, self.fat_sectors)
        struct.pack_into("<H", boot, 44, 0)
        struct.pack_into("<H", boot, 48, 1)   # FSInfo sector
        struct.pack_into("<H", boot, 50, 6)   # backup boot sector
        boot[64] = 0x80                      # drive number
        boot[66] = 0x29                      # extended boot signature
        struct.pack_into("<I", boot, 67, 0xA11C0DE5)
        boot[71:82] = volume_label.encode("ascii")[:11].ljust(11, b" ")
        boot[82:90] = b"FAT32   "
        boot[510:512] = b"\x55\xAA"
        self.image[self.sector_offset(0):self.sector_offset(0) + SECTOR] = boot
        # Backup boot sector and FSInfo copies.
        self.image[self.sector_offset(6):self.sector_offset(6) + SECTOR] = boot
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<I", fsinfo, 484, 0x61417272)
        struct.pack_into("<I", fsinfo, 488, self.clusters - ROOT_CLUSTER - 1)
        struct.pack_into("<I", fsinfo, 492, self.next_free)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)
        self.image[self.sector_offset(1):self.sector_offset(1) + SECTOR] = fsinfo
        self.image[self.sector_offset(7):self.sector_offset(7) + SECTOR] = fsinfo

    def flush_fat(self):
        fat_bytes = bytearray(self.fat_sectors * SECTOR)
        for index, value in enumerate(self.fat):
            struct.pack_into("<I", fat_bytes, index * 4, value & 0x0FFFFFFF)
        for copy in range(NUM_FATS):
            lba = RESERVED_SECTORS + copy * self.fat_sectors
            offset = self.sector_offset(lba)
            self.image[offset:offset + len(fat_bytes)] = fat_bytes

    def write_directory(self, cluster: int, entries: list):
        """Write a directory: a list of (name11, attr, cluster, size)."""
        data = bytearray(SECTORS_PER_CLUSTER * SECTOR)
        for index, (name, attr, entry_cluster, size) in enumerate(entries):
            if index >= 16:
                break
            base = index * 32
            data[base:base + 11] = name
            data[base + 11] = attr
            now = datetime.datetime.now()
            date = ((now.year - 1980) << 9) | (now.month << 5) | now.day
            time = (now.hour << 11) | (now.minute << 5) | (now.second // 2)
            struct.pack_into("<H", data, base + 14, time)
            struct.pack_into("<H", data, base + 16, date)
            struct.pack_into("<H", data, base + 18, date)
            struct.pack_into("<H", data, base + 20, entry_cluster >> 16)
            struct.pack_into("<H", data, base + 22, time)
            struct.pack_into("<H", data, base + 24, date)
            struct.pack_into("<H", data, base + 26, entry_cluster & 0xFFFF)
            struct.pack_into("<I", data, base + 28, size)
        self.write_cluster_data(cluster, bytes(data))

    def write_file(self, data: bytes) -> int:
        count = max(1, (len(data) + SECTORS_PER_CLUSTER * SECTOR - 1)
                    // (SECTORS_PER_CLUSTER * SECTOR))
        first = self.allocate(count)
        for i in range(count):
            chunk = data[i * SECTORS_PER_CLUSTER * SECTOR:
                         (i + 1) * SECTORS_PER_CLUSTER * SECTOR]
            self.write_cluster_data(first + i, chunk)
        return first


def short_name(name: str):
    """Convert a path component to an 8.3 directory entry name."""
    name = name.upper()
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    base = base[:8].ljust(8)
    ext = ext[:3].ljust(3)
    return (base + ext).encode("ascii")


def build_tree(writer: Fat32Writer, root_dir: str, root_cluster: int):
    """Recursively copy root_dir into the FAT32 image."""
    entries = []
    for name in sorted(os.listdir(root_dir)):
        path = os.path.join(root_dir, name)
        if os.path.isdir(path):
            cluster = writer.allocate(1)
            entries.append((short_name(name), ATTR_DIRECTORY, cluster, 0))
            sub = [(b".          ", ATTR_DIRECTORY, cluster, 0),
                   (b"..         ", ATTR_DIRECTORY, 0, 0)]
            index = 2
            for child in sorted(os.listdir(path)):
                child_path = os.path.join(path, child)
                if os.path.isdir(child_path):
                    child_cluster = writer.allocate(1)
                    sub.append((short_name(child), ATTR_DIRECTORY, child_cluster, 0))
                    build_nested(writer, child_path, child_cluster)
                else:
                    data = open(child_path, "rb").read()
                    file_cluster = writer.write_file(data)
                    sub.append((short_name(child), ATTR_ARCHIVE, file_cluster, len(data)))
                index += 1
            writer.write_directory(cluster, sub)
        else:
            data = open(path, "rb").read()
            file_cluster = writer.write_file(data)
            entries.append((short_name(name), ATTR_ARCHIVE, file_cluster, len(data)))
    writer.write_directory(root_cluster, entries)


def build_nested(writer: Fat32Writer, directory: str, cluster: int):
    entries = [(b".          ", ATTR_DIRECTORY, cluster, 0),
               (b"..         ", ATTR_DIRECTORY, 0, 0)]
    for child in sorted(os.listdir(directory)):
        path = os.path.join(directory, child)
        if os.path.isdir(path):
            child_cluster = writer.allocate(1)
            entries.append((short_name(child), ATTR_DIRECTORY, child_cluster, 0))
            build_nested(writer, path, child_cluster)
        else:
            data = open(path, "rb").read()
            file_cluster = writer.write_file(data)
            entries.append((short_name(child), ATTR_ARCHIVE, file_cluster, len(data)))
    writer.write_directory(cluster, entries)


def protective_mbr(image: bytearray):
    mbr = bytearray(SECTOR)
    entry = bytearray(16)
    entry[0] = 0x00                       # not bootable
    entry[1:4] = b"\x00\x02\x00"          # start CHS (LBA 1)
    entry[4] = 0xEE                       # GPT protective type
    entry[5:8] = b"\xFF\xFF\xFF"          # end CHS
    struct.pack_into("<I", entry, 8, 1)   # start LBA
    struct.pack_into("<I", entry, 12, min(IMAGE_SECTORS - 1, 0xFFFFFFFF))
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    image[0:SECTOR] = mbr


def build_gpt(image: bytearray, disk_guid: bytes):
    entries = bytearray(128 * 128)
    entry = bytearray(128)
    entry[0:16] = ESP_TYPE_GUID
    entry[16:32] = disk_guid
    struct.pack_into("<Q", entry, 32, ESP_START_LBA)
    struct.pack_into("<Q", entry, 40, ESP_START_LBA + ESP_SECTORS - 1)
    struct.pack_into("<Q", entry, 48, 0)
    entry[56:72] = "AxiomOS ESP".encode("utf-16-le")[:72].ljust(72, b"\x00")
    entries[0:128] = entry

    header = bytearray(SECTOR)
    header[0:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x00010000)
    struct.pack_into("<I", header, 12, 92)
    struct.pack_into("<I", header, 16, 0)
    struct.pack_into("<I", header, 20, 0)
    struct.pack_into("<Q", header, 24, 1)                 # my LBA
    struct.pack_into("<Q", header, 32, IMAGE_SECTORS - 1) # alternate LBA
    struct.pack_into("<Q", header, 40, 34)                # first usable
    struct.pack_into("<Q", header, 48, IMAGE_SECTORS - 34)
    header[56:72] = disk_guid
    struct.pack_into("<Q", header, 72, 2)                 # entries LBA
    struct.pack_into("<I", header, 80, 128)
    struct.pack_into("<I", header, 84, 128)
    struct.pack_into("<I", header, 88, crc32(bytes(entries)))
    struct.pack_into("<I", header, 16, 0)
    struct.pack_into("<I", header, 16, crc32(bytes(header[:92])))

    image[SECTOR:SECTOR * 2] = header
    image[SECTOR * 2:SECTOR * 2 + len(entries)] = entries

    # Backup GPT at the end of the disk.
    backup_entries_lba = IMAGE_SECTORS - 33
    backup_header = bytearray(header)
    struct.pack_into("<Q", backup_header, 24, IMAGE_SECTORS - 1)
    struct.pack_into("<Q", backup_header, 32, 1)
    struct.pack_into("<Q", backup_header, 72, backup_entries_lba)
    struct.pack_into("<I", backup_header, 16, 0)
    struct.pack_into("<I", backup_header, 16, crc32(bytes(backup_header[:92])))
    offset = (IMAGE_SECTORS - 1) * SECTOR
    image[offset:offset + SECTOR] = backup_header
    offset = backup_entries_lba * SECTOR
    image[offset:offset + len(entries)] = entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--esp", required=True, help="directory with ESP contents")
    parser.add_argument("--out", default="build/axiomos.img")
    args = parser.parse_args()

    image = bytearray(IMAGE_SECTORS * SECTOR)
    disk_guid = bytes.fromhex("a1b2c3d4e5f60718293a4b5c6d7e8f90")
    protective_mbr(image)
    build_gpt(image, disk_guid)

    writer = Fat32Writer(image, ESP_START_LBA, ESP_SECTORS)
    writer.format()
    build_tree(writer, args.esp, ROOT_CLUSTER)
    writer.flush_fat()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as handle:
        handle.write(image)
    print("image: {} ({} MiB, FAT32 ESP 64 MiB)".format(
        args.out, len(image) // (1024 * 1024)))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:               # pragma: no cover - CLI path
        print("mkdisk: {}".format(error), file=sys.stderr)
        sys.exit(1)
