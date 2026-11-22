#!/usr/bin/env python3
"""Convert a binary PPM (P6) framebuffer dump to PNG using zlib only."""

import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a binary PPM")
    # Skip the magic, then read three integers, tolerating comments.
    fields = []
    index = 2
    while len(fields) < 3:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            while index < len(data) and data[index:index + 1] not in b"\r\n":
                index += 1
            continue
        start = index
        while index < len(data) and not data[index:index + 1].isspace():
            index += 1
        fields.append(int(data[start:index]))
    index += 1
    width, height, _maxval = fields
    pixels = data[index:index + width * height * 3]
    return width, height, pixels


def write_png(path, width, height, pixels):
    rows = bytearray()
    stride = width * 3
    for y in range(height):
        rows.append(0)                              # filter type: none
        rows.extend(pixels[y * stride:(y + 1) * stride])

    def chunk(tag, payload):
        return (tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    body = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
            + chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(body)


def main():
    if len(sys.argv) != 3:
        print("usage: ppm2png.py in.ppm out.png", file=sys.stderr)
        return 1
    width, height, pixels = read_ppm(sys.argv[1])
    write_png(sys.argv[2], width, height, pixels)
    print("png: {}x{}".format(width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
