# TUS - Toasty Unix Software

*"Work everywhere, but work right."*

TUS is a UNIX-like operating system for the AMD64 (x86-64) architecture,
built with the [Limine](https://limine-bootloader.org) bootloader. It
targets every 64-bit machine, with a modular, clean and well-documented
codebase.

## Current features (v0.2.0)

- Boots via Limine (BIOS and UEFI), 64-bit long mode, higher-half kernel
- **Serial driver** - 16550 UART on COM1 (115200 8N1), debug mirror for
  all console output
- **Framebuffer console** - 8x16 text grid over the Limine-provided
  framebuffer, with a 2048-line scrollback (PageUp/PageDown) that
  preserves per-cell colors
- **Keyboard driver** - PS/2 scancode set 1, interrupt driven, Shift /
  Caps Lock / Ctrl handling and Caps Lock LED feedback
- **Physical memory manager** - bitmap frame allocator over the Limine
  memory map (only USABLE frames), with stats
- **Higher-half paging (VMM)** - extends Limine's own page tables on
  demand; `map_page` / `map_region` / `unmap_page`
- **kmalloc** - free-list kernel heap at `0xffffffff81000000` (64 MiB
  cap) with split/coalesce/krealloc
- **PIT timer** - 100 Hz IRQ0, `uptime`, `sleep`
- **Virtual file system** - ramfs tree: `/dev` (6 devices), `/tmp`,
  `/boot`, `/etc/motd`; fd table with stdin/stdout/stderr on `/dev/tty0`
- **Device nodes** - `/dev/fb0` (pixel read/write + `fbfill` ioctl),
  `/dev/tty0`, `/dev/kbd0`, `/dev/serial0`, `/dev/null`, `/dev/zero`
- **Syscalls** - POSIX-style `int $0x80` ABI (exit, read, write, open,
  close, ioctl, getpid, uptime, sleep, mkdir, unlink, readdir),
  dogfooded by tsh itself
- **tsh** - interactive shell: `help`, `echo`, `clear`, `ver`, `about`,
  `sysinfo`, `reboot`, `crash`, `ls`, `cat`, `mkdir`, `touch`, `rm`,
  `uptime`, `sleep`, `fbfill`
- **Interrupt handling** - full IDT, remapped PIC, register dump on CPU
  exceptions (kernel panic, incl. CR2 on page faults)

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
│   ├── drivers/           serial, keyboard, framebuffer, PIT
│   ├── mm/                PMM, VMM, kmalloc
│   ├── vfs/               VFS tree, fd table, device nodes
│   ├── syscall/           int $0x80 gate and dispatch
│   └── shell/             tsh, core commands, fs commands
├── tests/test_boot.py     automated boot + shell test (16 checks)
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

Click into the QEMU window and type `help`. Try:

```
cat /etc/motd
echo hello world > /tmp/greet
cat /tmp/greet
ls /dev
fbfill 336699
uptime
```

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

- Userspace: TUS init process, userspace tsh
- Scheduler + ring-3 enforcement of the syscall ABI
- Physical disk driver and a real filesystem
- Network stack
