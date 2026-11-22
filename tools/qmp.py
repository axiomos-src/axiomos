#!/usr/bin/env python3
"""Send a single command to the QEMU human monitor over a unix socket."""

import socket
import sys
import time


def main():
    if len(sys.argv) < 3:
        print("usage: qmp.py <socket> <command>", file=sys.stderr)
        return 1
    path, command = sys.argv[1], sys.argv[2]

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for attempt in range(20):
        try:
            sock.connect(path)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.25)
    else:
        print("qmp: cannot connect to {}".format(path), file=sys.stderr)
        return 1

    sock.sendall((command + "\n").encode())
    time.sleep(0.5)
    try:
        sock.settimeout(2.0)
        data = sock.recv(4096)
        print(data.decode(errors="replace"))
    except socket.timeout:
        pass
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
