<p align="center">
  <a href="https://axiomos-src.github.io">
    <img width="353" height="339" alt="AxiomOS" src="https://github.com/user-attachments/assets/b59fe536-9b5b-4159-af38-1e41fd851f94" />
  </a>
</p>

---

<h1 align="center">AxiomOS</h1>

<p align=center>
  <a href="https://axiomos-src.github.io>
    <img alt="AxiomOS Website" src="https://img.shields.io/badge/AxiomOS-Website-blue"></a>
  <a href="https://axiomos-src.github.io/downloads">
    <img alt="Download AxiomOS" src="https://img.shields.io/badge/AxiomOS-Downloads-green"></a>
  <a href="https//github.com/axiomos-src/axiomos">
    <img alt="AxiomOS Source Code" src="https://img.shields.io/badge/AxiomOS-Source_code-red"></a>
  <a href="https://github.com/axiomos-src/axiomos/blob/master/LICENSE">
    <img alt="License" src="https://img.shields.io/badge/license-MIT-blue"></a>
</p>

## What is AxiomOS?
AxiomOS is an open-source operating system built on the AxiomOS Kernel. The
project started on 31 July 2025 with one goal: to pave a new path for
self-developed operating systems instead of re-implementing an existing one.

> [!WARNING]
> AxiomOS is still in an early development stage. Installing it on real
> hardware may damage your data or leave the machine unbootable. Use a virtual
> machine or a spare disk.

- **Own design language, not a clone.** The GUI is built on *Aurora*, AxiomOS'
  own design system (`gui/aurora/`). It is deliberately not a port of any
  existing desktop style; every widget, window frame and palette is defined
  in-tree under the `AXAU_` / `axau_` prefix.
- **Mixed-language kernel.** C for the bulk of the kernel, NASM for the entry
  path, descriptor tables and ISR stubs, Rust for selected subsystems
  (`rust/`, `x86_64-unknown-none`, `no_std`).
- **UEFI-first boot.** The loader is a PE/COFF UEFI application that enumerates
  GOP modes, parses ACPI, builds the boot information block and hands control
  to `kernel.elf`.
- **Simple HVCI.** Code-integrity checking runs in software mode: page
  protection plus per-region hashing of kernel text, with violation counters
  exported to the GUI.
- **Around 115,000 lines of code**, counted with `tools/count.py`.

## Repository layout

| Path | Contents |
| --- | --- |
| `boot/uefi/` | UEFI loader: GOP, ACPI, FAT volume access, console |
| `arch/x86_64/` | Entry stub, GDT/TSS, IDT + ISR stubs, IRQ, CPU ops |
| `kernel/` | Core, MM, scheduler, IRQ, drivers, FS, networking, security, lib |
| `gui/` | Compositor, window manager, widgets, Aurora design system, apps |
| `rust/` | Rust crates linked into the kernel image |
| `tools/` | `count.py` (line counting), `check.py`, build and QEMU helpers |
| `docs/` | Documentation (work in progress) |

## Requirements

A Linux host is required to link the kernel. `ld` from a mingw toolchain can
only emit PE/COFF, so the kernel ELF cannot be produced on Windows.

- Ubuntu/Debian (tested on 22.04), x86-64
- `gcc` (freestanding, `-m64`), GNU `ld`, `objcopy`
- `nasm` (2.16 or newer)
- Rust toolchain with the `x86_64-unknown-none` target
- `make`, `python3`
- QEMU + OVMF (optional, for running)

Disk and memory: about 1 GB of free space is enough for a clean build tree;
2 GB of RAM is recommended. Running the machine needs at least 2 GB of guest
RAM.

### Installing the toolchain

```bash
sudo apt install gcc ld nasm make python3
# OVMF firmware for QEMU
sudo apt install ovmf qemu-system-x86

curl https://sh.rustup.rs -sS1f | sh -s -- -y --profile minimal
rustup target add x86_64-unknown-none
```

## Building

git clone https://github.com/axiomos-src/axiomos.git
cd axiomos

make kernel -j$(nproc)     # -> build/kernel.elf
```

Build types:

- `make kernel` — release kernel image (default flags in the `Makefile`)
- `make clean` — remove `build/`

The kernel is linked at `0x100000` with `arch/x86_64/linker.ld`; the resulting
image is a plain ELF64 executable loaded by the UEFI loader from the ESP.

The UEFI loader is built separately as a PE/COFF image, for example with
`tools/build_loader.ps1` on a mingw host. It is copied into
`build/esp/EFI/BOOT/BOOTX64.EFI` next to `build/esp/axiomos/kernel.elf`.
```

## Running under QEMU

```
qemu-system-x86_64 \
  -machine q35 -m 2048 -smp 2 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=build/ovmf/code.fd \
  -drive if=pflash,format=raw,unit=1,file=build/ovmf/vars.fd \
  -drive format=raw,file=fat:rw:build/esp \
  -serial stdio
```

Use `-display gtk` (or `-display sdl`) instead of `-display none` to see the
desktop. A successful boot ends with:

```
gui: desktop ready
boot: completed in 440 ms
```

## Line counting

```
python3 tools/count.py --detail
```

Reports `code`, `tools`, `docs` and `total` counts against the project target.

## Bootable images

Prebuilt images are named `axiomos-<version>-<YYYYMMDD>.iso`, for example
`axiomos-1.1.27-20261114.iso` for version 1.1.27 built on 14 November 2026.
They are published at [https://axiomos.github.io/downloads](https://axiomos.github.io/downloads).

## License

MIT License. See [LICENSE](LICENSE).

## More information

- Project page: [https://axiomos-src.github.io/moreinfo](https://axiomos-src.github.io/moreinfo)
- Documentation: [https://github.com/axiomos-src/axiomos/tree/master/docs](https://github.com/axiomos-src/axiomos/tree/master/docs)
