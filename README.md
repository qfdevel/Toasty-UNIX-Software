# TUS - Toasty Unix Software

*"Work everywhere, but work right."*

TUS is a UNIX-like operating system for the AMD64 (x86-64) architecture,
built with the [Limine](https://limine-bootloader.org) bootloader. It
targets every 64-bit machine, with a modular, clean and well-documented
codebase.

## Current features (v0.1.0)

- Boots via Limine (BIOS and UEFI), 64-bit long mode, higher-half kernel
- **Serial driver** - 16550 UART on COM1 (115200 8N1), used as the
  debug mirror for all console output
- **Framebuffer console** - 8x16 text grid over the Limine-provided
  framebuffer (the foundation of the future `/dev/fb0`)
- **Keyboard driver** - PS/2 scancode set 1, interrupt driven, with
  Shift / Caps Lock / Ctrl handling and Caps Lock LED feedback
- **tsh** - the TUS shell, an interactive command line with built-in
  commands: `help`, `echo`, `clear`, `ver`, `about`, `sysinfo`,
  `reboot`, `crash` (exception handler demo)
- **Interrupt handling** - full IDT, remapped PIC, register dump on CPU
  exceptions (kernel panic)

## Project layout

```
TOS/
├── limine-bin/            bootloader binaries (provided)
├── include/limine.h       Limine boot protocol header
├── kernel/
│   ├── linker.ld          higher-half linker script
│   ├── main.c             entry point, boot sequence
│   ├── arch/x86_64/       CPU-specific: IDT, PIC, CPUID, port I/O
│   ├── core/              klib (mem + printf), console, bootinfo
│   ├── drivers/           serial, keyboard, framebuffer console
│   └── shell/             tsh and its command table
├── tests/test_boot.py     automated boot + shell test
├── Makefile
└── limine.conf
```

## Building and running

Requirements (Arch Linux):

```
sudo pacman -S gcc nasm make qemu-desktop limine xorriso
```

Build and boot in QEMU:

```
make run
```

Click into the QEMU window and type `help`.

Automated headless test (boots, types into the virtual keyboard,
verifies the shell responses):

```
make test
```

## Backup policy

Before any significant change, a full backup of the project is taken:

```
tar czf ~/TOS-backup-YYYYMMDD-HHMM.tar.gz /home/quake/TOS
```

## Roadmap

- Physical memory manager and higher-half paging
- PIT timer and a scheduler
- Virtual file system, `/dev/fb0` framebuffer device
- Userspace: TUS init process, userspace tsh
