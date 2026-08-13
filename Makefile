# TUS - Toasty Unix Software
#
# Targets:
#   make          - build kernel.elf
#   make iso      - build tus.iso (bootable hybrid BIOS/UEFI image)
#   make run      - build and boot in QEMU (windowed)
#   make run-smp  - boot with 4 virtual CPUs (boot splash shows 4 toasts)
#   make test     - build and run the automated boot test
#   make clean    - remove build artifacts
#
# Layout:
#   kernel/       - kernel sources (compiled into build/kernel/)
#   rootfs/       - staging dir for the root filesystem image
#   rootfs.img    - ustar tar of rootfs/, loaded by Limine as a module
#   build/        - all compiler output (objects, deps, test objects)
#   musl-out/     - ported musl libc (headers + libc.a + crt)

CC      := gcc
LD      := ld

# musl libc (userspace C library, ported to the TUS syscall ABI).
# `make musl` (or the first kernel build) configures, builds and
# installs musl-1.2.6 into musl-out/ (headers + libc.a + crt).
MUSL_DIR := sources/musl-1.2.6
MUSL_OUT := musl-out
MUSL_INC := $(MUSL_OUT)/usr/include
MUSL_LIB := $(MUSL_OUT)/usr/lib

# -m64            64-bit code (long mode, no paging setup by us)
# -ffreestanding  no hosted runtime assumptions
# -fno-stack-protector  no SSP (no canaries in kernel mode)
# -fno-pic        fixed higher-half addresses
# -mcmodel=kernel   kernel runs in the negative 2 GiB (0xffffffff80000000)
# -mno-red-zone   interrupt handlers must not clobber the red zone
# -mgeneral-regs-only  keep FPU/SSE state untouched in kernel mode
CFLAGS  := -m64 -ffreestanding -fno-stack-protector -fno-pic \
           -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -O2 -Wall -Wextra \
           -std=gnu11 -Iinclude -Ikernel

LDFLAGS := -m elf_x86_64 -T kernel/linker.ld

BUILD       := build
KERNEL_SRCS := $(shell find kernel -name '*.c')
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_SRCS))
DEPS        := $(KERNEL_OBJS:.o=.d)

# Root filesystem: rootfs/ is tarred into rootfs.img (ustar format,
# parsed by kernel/vfs/rootfs.c). The user programs are built straight
# into rootfs/boot/, so the ISO ships exactly the files the running
# system sees at /boot.
ROOTFS_DIR   := rootfs
ROOTFS_IMG   := rootfs.img
ROOTFS_FILES := $(shell find $(ROOTFS_DIR) -type f 2>/dev/null)

# Static x86-64 user programs (linked at 0x10000000, entry _start).
USER_CFLAGS := -m64 -ffreestanding -fno-stack-protector -fno-pic \
               -mno-red-zone -mgeneral-regs-only -O2
USER_LDFLAGS := -m elf_x86_64 -static -e _start -Ttext 0x10000000

USER_ELFS := $(ROOTFS_DIR)/boot/hello.elf \
             $(ROOTFS_DIR)/boot/enforce.elf \
             $(ROOTFS_DIR)/boot/musl_hello.elf \
             $(ROOTFS_DIR)/boot/kilo.elf

.PHONY: all iso run run-smp test clean clean-musl musl

all: kernel.elf

musl: $(MUSL_LIB)/libc.a

$(MUSL_LIB)/libc.a:
	cd $(MUSL_DIR) && CC=gcc ./configure --target=x86_64-unknown-tus \
		--disable-shared --prefix=/usr
	$(MAKE) -C $(MUSL_DIR) -j4 AR=ar RANLIB=ranlib
	$(MAKE) -C $(MUSL_DIR) install DESTDIR=$(CURDIR)/$(MUSL_OUT)

kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ---- user programs (built into the rootfs staging dir) ----

# Plain freestanding test programs (no libc).
$(ROOTFS_DIR)/boot/hello.elf: tests/hello.c
	@mkdir -p $(ROOTFS_DIR)/boot $(BUILD)/tests
	$(CC) $(USER_CFLAGS) -c $< -o $(BUILD)/tests/hello.o
	$(LD) $(USER_LDFLAGS) -o $@ $(BUILD)/tests/hello.o

$(ROOTFS_DIR)/boot/enforce.elf: tests/enforce.c
	@mkdir -p $(ROOTFS_DIR)/boot $(BUILD)/tests
	$(CC) $(USER_CFLAGS) -c $< -o $(BUILD)/tests/enforce.o
	$(LD) $(USER_LDFLAGS) -o $@ $(BUILD)/tests/enforce.o

# Programs linked against the ported musl libc (crt1.o + libc.a).
# Compiles against the musl headers (-nostdinc), like musl_hello.elf.
$(ROOTFS_DIR)/boot/musl_hello.elf: tests/musl_hello.c $(MUSL_LIB)/libc.a
	@mkdir -p $(ROOTFS_DIR)/boot $(BUILD)/tests
	$(CC) $(USER_CFLAGS) -nostdinc -I$(MUSL_INC) -c $< -o $(BUILD)/tests/musl_hello.o
	$(LD) $(USER_LDFLAGS) -o $@ \
		$(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o $(BUILD)/tests/musl_hello.o \
		-L$(MUSL_LIB) -lc $(MUSL_LIB)/crtn.o

# kilo: a real terminal editor, unmodified, running as a ring-3 musl
# program (termios, TIOCGWINSZ, raw input, ANSI output - all provided
# by the kernel, see v0.6.0).
$(ROOTFS_DIR)/boot/kilo.elf: sources/kilo/kilo.c $(MUSL_LIB)/libc.a
	@mkdir -p $(ROOTFS_DIR)/boot $(BUILD)/tests
	$(CC) $(USER_CFLAGS) -nostdinc -I$(MUSL_INC) -c $< -o $(BUILD)/tests/kilo.o
	$(LD) $(USER_LDFLAGS) -o $@ \
		$(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o $(BUILD)/tests/kilo.o \
		-L$(MUSL_LIB) -lc $(MUSL_LIB)/crtn.o

# ---- rootfs image ----

$(ROOTFS_IMG): $(USER_ELFS) $(ROOTFS_FILES)
	tar --format=ustar -C $(ROOTFS_DIR) -cf $@ .

# Header dependency tracking: rebuild objects when the headers they
# include change (without this, editing a .h silently tests a stale
# kernel).
-include $(DEPS)

# Assemble the hybrid BIOS/UEFI ISO from the limine-bin/ files.
# rootfs.img is passed to the kernel as a Limine module (see
# limine.conf: module_path) and becomes the initial root filesystem.
iso: kernel.elf $(ROOTFS_IMG)
	rm -rf iso_root
	mkdir -p iso_root/boot iso_root/EFI/BOOT
	cp kernel.elf iso_root/boot/kernel.elf
	cp $(ROOTFS_IMG) iso_root/boot/rootfs.img
	cp limine.conf iso_root/boot/limine.conf
	cp limine-bin/limine-bios.sys limine-bin/limine-bios-cd.bin \
	   limine-bin/limine-uefi-cd.bin iso_root/boot/
	cp limine-bin/BOOTX64.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -b boot/limine-bios-cd.bin -no-emul-boot \
		-boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o tus.iso
	./limine-bin/limine bios-install tus.iso

run: iso
	qemu-system-x86_64 -cdrom tus.iso -m 512M -no-reboot -serial stdio

# The boot splash draws one toast per CPU; boot with several vCPUs to
# see the row of toasts (APs are parked by the kernel, see main.c).
run-smp: iso
	qemu-system-x86_64 -cdrom tus.iso -m 512M -smp 4 -no-reboot -serial stdio

test: iso
	python3 tests/test_boot.py

clean:
	rm -rf $(BUILD) kernel.elf tus.iso iso_root $(ROOTFS_IMG)
	rm -f $(ROOTFS_DIR)/boot/*.elf

# Remove the musl build too (keeps the source tree, drops obj/ lib/
# and the installed musl-out/).
clean-musl:
	$(MAKE) -C $(MUSL_DIR) clean
	rm -rf $(MUSL_OUT)
