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

# Embedded test program for the ELF loader: tests/hello.elf is
# converted to an object file (binary blob) and linked into the
# kernel, then exposed at /boot/hello.elf in the VFS.
TEST_ELF_SRC := tests/hello.c
TEST_ELF     := tests/hello.elf
TEST_ELF_OBJ := tests/hello.o
TEST_ELF_BLOB := tests/hello_blob.o

KERNEL_OBJS += $(TEST_ELF_BLOB)

.PHONY: all iso run test clean

all: kernel.elf

kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST_ELF): $(TEST_ELF_SRC)
	$(CC) -m64 -ffreestanding -fno-stack-protector -fno-pic \
		-mno-red-zone -mgeneral-regs-only -O2 -c $(TEST_ELF_SRC) -o $(TEST_ELF_OBJ)
	$(LD) -m elf_x86_64 -static -e _start -Ttext 0x400000 -o $@ $(TEST_ELF_OBJ)

# ld -r -b binary turns a file into an object with symbols
# _binary_<name>_start/_end; the kernel copies it into the VFS.
$(TEST_ELF_BLOB): $(TEST_ELF)
	$(LD) -r -b binary $(TEST_ELF) -o $@

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
	rm -rf $(KERNEL_OBJS) $(DEPS) kernel.elf tus.iso iso_root $(TEST_ELF) $(TEST_ELF_OBJ)
