#!/usr/bin/env python3
"""Minimal VNC (RFB 3.8) client: grab one framebuffer update and write a PNG.

Usage:
    python vncshot.py <host> <port> <out.png> [--key PASSWORD]

Implements just enough of the protocol for screenshots:
Raw encoding only, no clipboard, no input events.
"""

import socket
import struct
import sys
import time
import zlib


def recv_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise RuntimeError("vnc: connection closed")
        data += chunk
    return data


def reverse_bits(byte):
    out = 0
    for i in range(8):
        if byte & (1 << i):
            out |= 1 << (7 - i)
    return out


def des_encrypt_block(key, block):
    """DES ECB encrypt of one 8 byte block, used only for VNC auth."""
    # Expand key to 16 subkeys of 48 bits.
    pc1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
           10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
           63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
           14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4]
    pc2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10,
           23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
           41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
           44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
    shifts = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
    ip = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
          62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
          57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
          61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
    fp = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
          38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
          36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
          34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25]
    e = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13,
         12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
         24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1]
    p = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
         2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
    sboxes = [
        [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
         0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
         4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
         15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
        [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
         3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
         0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
         13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
        [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
         13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
         13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
         1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
        [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
         13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
         10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
         3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
        [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
         14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
         4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
         11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
        [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
         10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
         9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
         4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
        [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
         13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
         1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
         6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
        [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
         1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
         7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
         2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
    ]

    key64 = [reverse_bits(b) for b in key]
    keybits = [key64[i // 8] >> (7 - (i % 8)) & 1 for i in range(64)]
    cd = [keybits[i - 1] for i in pc1]
    subkeys = []
    for shift in shifts:
        cd[:28] = cd[shift:] + cd[:shift]
        cd[28:] = cd[28 + shift:] + cd[28:28 + shift]
        subkeys.append([cd[i - 1] for i in pc2])

    blockbits = [reverse_bits(b) for b in block]
    bits = [blockbits[i // 8] >> (7 - (i % 8)) & 1 for i in range(64)]
    data = [bits[i - 1] for i in ip]
    left, right = data[:32], data[32:]
    for k in subkeys:
        expanded = [right[i - 1] for i in e]
        mixed = [expanded[i] ^ k[i] for i in range(48)]
        sresult = []
        for box in range(8):
            chunk = mixed[box * 6:box * 6 + 6]
            row = chunk[0] * 2 + chunk[5]
            col = chunk[1] * 8 + chunk[2] * 4 + chunk[3] * 2 + chunk[4]
            sresult.extend([(sboxes[box][row * 16 + col] >> (3 - i)) & 1 for i in range(4)])
        permuted = [sresult[i - 1] for i in p]
        left, right = right, [left[i] ^ permuted[i] for i in range(32)]
    final = right + left
    out_bits = [final[i - 1] for i in fp]
    cipher = [reverse_bits(sum(out_bits[i * 8 + j] << (7 - j) for j in range(8))) for i in range(8)]
    return bytes(cipher)


def vnc_auth(sock, password):
    # 16 byte challenge, encrypt with DES using the first 8 characters padded
    # with NULs, each byte bit-reversed (VNC legacy quirk).
    challenge = recv_exact(sock, 16)
    key = password.encode("latin-1")[:8].ljust(8, b"\x00")
    response = b""
    for i in range(2):
        block = bytes(reverse_bits(b) for b in challenge[i * 8:(i + 1) * 8])
        response += des_encrypt_block(key, block)
    sock.sendall(response)
    state = recv_exact(sock, 4)
    if struct.unpack(">I", state)[0] != 0:
        raise RuntimeError("vnc: authentication failed")


def write_png(path, width, height, pixels):
    rows = b""
    for y in range(height):
        rows += b"\x00" + pixels[y * width * 4:(y + 1) * width * 4]
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(data)


def main():
    host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    password = ""
    if "--key" in sys.argv:
        password = sys.argv[sys.argv.index("--key") + 1]
    sock = socket.create_connection((host, port), timeout=10)
    version = recv_exact(sock, 12)
    sock.sendall(b"RFB 003.008\n")
    count = recv_exact(sock, 1)[0]
    types = recv_exact(sock, count)
    if 2 in types:
        sock.sendall(b"\x02")
        vnc_auth(sock, password)
    elif 1 in types:
        sock.sendall(b"\x01")
        state = recv_exact(sock, 4)
        if struct.unpack(">I", state)[0] != 0:
            raise RuntimeError("vnc: security handshake failed")
    else:
        raise RuntimeError("vnc: no supported security type %r" % list(types))
    sock.sendall(b"\x01")                       # shared session
    width, height = struct.unpack(">HH", recv_exact(sock, 4))
    recv_exact(sock, 16)                        # pixel format
    namelen = struct.unpack(">I", recv_exact(sock, 4))[0]
    recv_exact(sock, namelen)
    # Pixel format: 32 bpp, depth 24, little endian, true colour.
    sock.sendall(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
    sock.sendall(struct.pack(">BBHI", 2, 0, 1, 0))   # SetEncodings: raw only
    sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))
    pixels = bytearray(width * height * 4)
    while True:
        msg = recv_exact(sock, 1)[0]
        if msg == 0:
            recv_exact(sock, 1)
            rects = struct.unpack(">H", recv_exact(sock, 2))[0]
            for _ in range(rects):
                x, y, w, h, enc = struct.unpack(">HHHHi", recv_exact(sock, 12))
                if enc != 0:
                    raise RuntimeError("vnc: unsupported encoding %d" % enc)
                raw = recv_exact(sock, w * h * 4)
                for row in range(h):
                    dst = ((y + row) * width + x) * 4
                    pixels[dst:dst + w * 4] = raw[row * w * 4:(row + 1) * w * 4]
            break
        elif msg == 1:
            result = recv_exact(sock, 4)
            print("vnc: set colour map", result)
            recv_exact(sock, 4 * 256 * 3)
        elif msg == 3:
            recv_exact(sock, 4 + struct.unpack(">H", recv_exact(sock, 2))[0])
            continue
    sock.close()
    write_png(out, width, height, bytes(pixels))
    print("wrote %s (%dx%d)" % (out, width, height))


if __name__ == "__main__":
    main()
