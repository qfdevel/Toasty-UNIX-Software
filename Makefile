# TUS - Toasty Unix Software
#
# Targets:
#   make          - build kernel.elf
#   make iso      - build tus.iso (bootable hybrid BIOS/UEFI image)
#   make run      - build and boot in QEMU (windowed)
#   make test     - build and run the automated boot test
#   make clean    - remove build artifacts

CC      := gcc
LD      := ld

# musl libc (userspace C library, ported to the TUS syscall ABI).
# `make musl` (or the first kernel build) configures, builds and
# installs musl-1.2.6 into musl-out/ (headers + libc.a + crt).
MUSL_DIR := musl-1.2.6
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

KERNEL_SRCS := $(shell find kernel -name '*.c')
KERNEL_OBJS := $(KERNEL_SRCS:.c=.o)
DEPS        := $(KERNEL_OBJS:.o=.d)

# Embedded test programs for the ELF loader: tests/hello.elf,
# tests/enforce.elf and the musl-linked tests/musl_hello.elf are
# converted to object files (binary blobs) and linked into the
# kernel, then exposed at /boot/ in the VFS.
TEST_ELFS := tests/hello.elf tests/enforce.elf tests/musl_hello.elf
TEST_ELF_BLOBS := $(TEST_ELFS:.elf=_blob.o)

KERNEL_OBJS += $(TEST_ELF_BLOBS)

.PHONY: all iso run test clean musl

all: kernel.elf

musl: $(MUSL_LIB)/libc.a

$(MUSL_LIB)/libc.a:
	cd $(MUSL_DIR) && CC=gcc ./configure --target=x86_64-unknown-tus \
		--disable-shared --prefix=/usr
	$(MAKE) -C $(MUSL_DIR) -j4 AR=ar RANLIB=ranlib
	$(MAKE) -C $(MUSL_DIR) install DESTDIR=$(CURDIR)/$(MUSL_OUT)
kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Each test ELF: freestanding static x86-64 image linked at
# 0x10000000 (the user half). Entry point is _start.
tests/%.elf: tests/%.c
	$(CC) -m64 -ffreestanding -fno-stack-protector -fno-pic \
		-mno-red-zone -mgeneral-regs-only -O2 -c $< -o $*.o
	$(LD) -m elf_x86_64 -static -e _start -Ttext 0x10000000 -o $@ $*.o

# tests/musl_hello.elf: a real C program linked against the ported
# musl libc (crt1.o + libc.a). Compiles against the musl headers
# (-nostdinc) and links statically at 0x10000000 like the others.
tests/musl_hello.elf: tests/musl_hello.c $(MUSL_LIB)/libc.a
	$(CC) -m64 -ffreestanding -fno-stack-protector -fno-pic \
		-mno-red-zone -mgeneral-regs-only -O2 -nostdinc \
		-I$(MUSL_INC) -c $< -o tests/musl_hello.o
	$(LD) -m elf_x86_64 -static -e _start -Ttext 0x10000000 -o $@ \
		$(MUSL_LIB)/crt1.o $(MUSL_LIB)/crti.o tests/musl_hello.o \
		-L$(MUSL_LIB) -lc $(MUSL_LIB)/crtn.o

# ld -r -b binary turns a file into an object with symbols
# _binary_<name>_start/_end; the kernel copies it into the VFS.
$(TEST_ELF_BLOBS): %_blob.o: %.elf
	$(LD) -r -b binary $< -o $@

# Header dependency tracking: rebuild objects when the headers they
# include change (without this, editing a .h silently tests a stale
# kernel).
-include $(DEPS)

# Assemble the hybrid BIOS/UEFI ISO from the limine-bin/ files.
iso: kernel.elf
	rm -rf iso_root
	mkdir -p iso_root/boot iso_root/EFI/BOOT
	cp kernel.elf iso_root/boot/kernel.elf
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

test: iso
	python3 tests/test_boot.py

clean:
	rm -rf $(KERNEL_OBJS) $(DEPS) kernel.elf tus.iso iso_root \
		$(TEST_ELFS) tests/hello.o tests/enforce.o tests/musl_hello.o

# Remove the musl build too (keeps the source tree, drops obj/ lib/
# and the installed musl-out/).
clean-musl:
	$(MAKE) -C $(MUSL_DIR) clean
	rm -rf $(MUSL_OUT)
