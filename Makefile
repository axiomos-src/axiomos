# AxiomOS build system.
#
# Toolchain:
#   gcc (host x86_64-linux-gnu)  - freestanding kernel objects, System V ABI
#   nasm                        - assembly stubs
#   x86_64-w64-mingw32-gcc       - UEFI loader (PE/COFF, MS x64 ABI)
#   rustc / cargo               - Rust components (x86_64-unknown-none)
#
# Targets:
#   make            build kernel.elf, BOOTX64.EFI and the disk image
#   make kernel     kernel only
#   make loader     UEFI loader only
#   make rust       Rust static library
#   make image      GPT disk image (build/axiomos.img)
#   make iso        El Torito EFI ISO (build/axiomos.iso)
#   make run        boot in QEMU with OVMF (serial on stdio)
#   make shot       boot headless, capture a screenshot to build/shot.ppm
#   make count      count source lines by language
#   make clean

ARCH      := x86_64
BUILD     := build
CC        := gcc
LD        := ld
NASM      := nasm
MINGW     := x86_64-w64-mingw32-gcc

CFLAGS := -m64 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
          -mno-red-zone -mno-sse -mno-mmx -mno-sse2 -mcmodel=large -O2 \
          -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -fno-tree-loop-distribute-patterns -fno-omit-frame-pointer \
          -Ikernel/include -Ikernel -Iboot -Igui -Igui/ui -Igui/aurora -Iarch

LDFLAGS := -T arch/x86_64/linker.ld -nostdlib

UEFI_CFLAGS := -ffreestanding -fno-stack-protector -fno-stack-check \
               -mno-red-zone -mno-stack-arg-probe -O2 -Wall -Wextra \
               -Wno-unused-parameter -Iboot/uefi -Iboot

C_SOURCES   := $(shell find kernel gui arch/x86_64 -name '*.c' 2>/dev/null)
ASM_SOURCES := $(shell find arch/x86_64 -name '*.asm' 2>/dev/null)
UEFI_SOURCES := $(wildcard boot/uefi/*.c)

C_OBJECTS    := $(patsubst %.c,$(BUILD)/obj/%.o,$(C_SOURCES))
ASM_OBJECTS  := $(patsubst %.asm,$(BUILD)/obj/%.o,$(ASM_SOURCES))
UEFI_OBJECTS := $(patsubst %.c,$(BUILD)/uefi/%.o,$(UEFI_SOURCES))
UEFI_EXTRA   := $(BUILD)/uefi/jump.o

RUST_LIB := $(BUILD)/rust/libaxi_kernel.a
RUST_CRATES := $(shell ls -d rust/*/ 2>/dev/null)

OBJ_DIRS := $(sort $(dir $(C_OBJECTS) $(ASM_OBJECTS) $(UEFI_OBJECTS)))

.PHONY: all kernel loader rust image iso run shot clean count font check

all: kernel loader rust image
	@echo "== AxiomOS build complete =="

$(OBJ_DIRS):
	@mkdir -p $@

$(BUILD)/obj/%.o: %.c | $(OBJ_DIRS)
	@printf "  CC    %s\n" $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.asm | $(OBJ_DIRS)
	@printf "  ASM   %s\n" $<
	@$(NASM) -f elf64 $< -o $@

kernel: $(BUILD)/kernel.elf
$(BUILD)/kernel.elf: $(C_OBJECTS) $(ASM_OBJECTS) $(RUST_LIB) arch/x86_64/linker.ld
	@printf "  LD    %s\n" $@
	@$(LD) $(LDFLAGS) -o $@ $(C_OBJECTS) $(ASM_OBJECTS) $(RUST_LIB)
	@echo "  kernel size: $$(stat -c%s $@) bytes"
	@$(LD) -T arch/x86_64/linker.ld -o $@.sym $(C_OBJECTS) $(ASM_OBJECTS) $(RUST_LIB) 2>/dev/null || true

loader: $(BUILD)/BOOTX64.EFI
$(BUILD)/BOOTX64.EFI: $(UEFI_OBJECTS) $(UEFI_EXTRA)
	@printf "  LINK  %s\n" $@
	@$(MINGW) -nostdlib -Wl,--subsystem,10 -Wl,-e,efi_main -o $@ $^

$(BUILD)/uefi/%.o: %.c | $(OBJ_DIRS)
	@printf "  CC    %s (uefi)\n" $<
	@$(MINGW) $(UEFI_CFLAGS) -c $< -o $@

$(BUILD)/uefi/jump.o: boot/uefi/jump.asm | $(OBJ_DIRS)
	@$(NASM) -f win64 $< -o $@

# The splash font is generated from the console PSF font.
font: boot/uefi/font5x7.c
boot/uefi/font5x7.c: tools/mkfont.py
	@python3 tools/mkfont.py

# --------------------------------------------------------------------
# Rust components
# --------------------------------------------------------------------
rust: $(RUST_LIB)
RUSTFLAGS_RUST := -C panic=abort -C code-model=large -C relocation-model=static \
                  -C opt-level=2 -C debuginfo=0 -C force-frame-pointers=yes

$(RUST_LIB): $(shell find rust -name '*.rs' -o -name 'Cargo.toml' 2>/dev/null)
	@printf "  CARGO Rust staticlib\n"
	@RUSTFLAGS="$(RUSTFLAGS_RUST)" RUSTC_BOOTSTRAP=1 \
	  cargo build --release --target x86_64-unknown-none \
	  --manifest-path rust/Cargo.toml --quiet
	@mkdir -p $(BUILD)/rust
	@cp rust/target/x86_64-unknown-none/release/libaxi_kernel.a $@

# --------------------------------------------------------------------
# Images
# --------------------------------------------------------------------
$(BUILD)/esp/EFI/BOOT/BOOTX64.EFI: $(BUILD)/BOOTX64.EFI
	@mkdir -p $(BUILD)/esp/EFI/BOOT
	@cp $< $@

image: $(BUILD)/axiomos.img
$(BUILD)/axiomos.img: $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI $(BUILD)/kernel.elf $(BUILD)/esp/boot.nsh
	@printf "  IMG   %s\n" $@
	@python3 tools/mkdisk.py --esp $(BUILD)/esp --out $@

$(BUILD)/esp/boot.nsh:
	@mkdir -p $(BUILD)/esp/axiomos
	@printf "# AxiomOS boot configuration\nkernel=/axiomos/kernel.elf\nhvci=auto\n" > $@
	@cp $(BUILD)/kernel.elf $(BUILD)/esp/axiomos/kernel.elf

iso: $(BUILD)/axiomos.iso
$(BUILD)/axiomos.iso: $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI $(BUILD)/kernel.elf
	@mkdir -p $(BUILD)/esp/axiomos
	@cp $(BUILD)/kernel.elf $(BUILD)/esp/axiomos/kernel.elf
	@xorriso -as mkisofs -o $@ -V AXIOMOS -e EFI/BOOT/BOOTX64.EFI \
	  -no-emul-boot -boot-load-size 4 -isohybrid-gpt-basdat $(BUILD)/esp 2>/dev/null
	@echo "  ISO   $@"

# --------------------------------------------------------------------
# Running
# --------------------------------------------------------------------
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
MEMORY    ?= 512
QEMU      ?= qemu-system-x86_64

$(BUILD)/OVMF_VARS.fd:
	@cp $(OVMF_VARS) $@

run: $(BUILD)/axiomos.img $(BUILD)/OVMF_VARS.fd
	@$(QEMU) -m $(MEMORY) -machine q35,accel=tcg -smp 2 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive file=$(BUILD)/axiomos.img,format=raw,if=none,id=disk0 \
	  -device ich9-ahci,id=ahci -device ide-hd,drive=disk0,bus=ahci.0 \
	  -device virtio-net-pci,netdev=n0 -netdev user,id=n0 \
	  -serial stdio -display gtk

# Headless boot with a screenshot after a few seconds.  Useful in CI and in
# environments without a display server.
shot: $(BUILD)/axiomos.img $(BUILD)/OVMF_VARS.fd
	@bash tools/shot.sh

# --------------------------------------------------------------------
# Statistics and housekeeping
# --------------------------------------------------------------------
count:
	@python3 tools/count.py

check:
	@python3 tools/check.py

clean:
	@rm -rf $(BUILD)
	@echo "cleaned"

