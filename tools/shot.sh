#!/usr/bin/env bash
# Boot AxiomOS headlessly, capture the serial log and a framebuffer dump.
#
# The QEMU monitor is exposed on a unix socket so that the script can issue
# `screendump` without a display server.  Outputs:
#   build/serial.log   - kernel log from the serial port
#   build/shot.ppm     - framebuffer as PPM
#   build/shot.png     - framebuffer converted to PNG
set -u

cd "$(dirname "$0")/.."

MONITOR=build/qemu-monitor.sock
SERIAL=build/serial.log
PPM=build/shot.ppm
SECONDS_TO_BOOT=${SECONDS_TO_BOOT:-25}

: > "$SERIAL"
rm -f "$MONITOR" "$PPM"

qemu-system-x86_64 \
  -m 512 -machine q35,accel=tcg -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive file=build/axiomos.img,format=raw,if=none,id=disk0 \
  -device ich9-ahci,id=ahci -device ide-hd,drive=disk0,bus=ahci.0 \
  -device VGA,vgamem_mb=16 \
  -display none -vnc 127.0.0.1:9 \
  -monitor unix:$MONITOR,server,nowait \
  -serial file:$SERIAL \
  -no-reboot &
QEMU_PID=$!

sleep "$SECONDS_TO_BOOT"

python3 tools/qmp.py "$MONITOR" "screendump $PPM" || true

sleep 1
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

if [ -f "$PPM" ]; then
  python3 tools/ppm2png.py "$PPM" build/shot.png
  echo "screenshot: build/shot.png"
else
  echo "no screenshot produced" >&2
fi
echo "serial log: $SERIAL"
