# TUS - Toasty Unix Software

*"Work everywhere, but work right."*

TUS is a UNIX-like operating system for the AMD64 (x86-64) architecture,
built with the [Limine](https://limine-bootloader.org) bootloader. It
targets every 64-bit machine, with a modular, clean and well-documented
codebase.

## Current features (v0.7.0)

- Boots via Limine (BIOS and UEFI), 64-bit long mode, higher-half kernel
- **Boot splash** - one toast per CPU, like Linux's Tux logos: the
  kernel counts the CPUs (Limine MP feature) and draws that many
  scaled toasts across the top of the framebuffer, with the boot log
  scrolling below them. The logo is read at runtime from the root
  filesystem (`/logo.ppm`, PPM decoded by the kernel - no embedded
  arrays), so swapping the image is a one-file change.
- **Root filesystem image** - `rootfs.img` (ustar tar) ships inside
  `tus.iso` as a Limine module and is mounted at boot; the user
  programs live in `/boot` and the boot logo at `/logo.ppm`. Staging
  dir: `rootfs/`
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
  demand; per-task address spaces (private user half, shared kernel
  half), `map_page` / `map_region` / `unmap_page`
- **kmalloc** - free-list kernel heap at `0xffffffff81000000` (64 MiB
  cap) with split/coalesce/krealloc; multi-page blocks are released
  frame by frame on `kfree`
- **PIT timer** - 100 Hz IRQ0, `uptime`, `sleep`
- **Virtual file system** - ramfs tree: `/dev` (6 devices), `/tmp`,
  `/boot`, `/etc/motd`; fd table with stdin/stdout/stderr on `/dev/tty0`
- **Device nodes** - `/dev/fb0` (pixel read/write + `fbfill` ioctl),
  `/dev/tty0`, `/dev/kbd0`, `/dev/serial0`, `/dev/null`, `/dev/zero`
- **Syscalls** - POSIX-style `int $0x80` ABI (exit, read, write, open,
  close, ioctl, getpid, uptime, sleep, mkdir, unlink, readdir, mmap,
  munmap, arch_prctl, writev, time, ftruncate), dogfooded by tsh itself
- **Scheduler** - round-robin preemptive multitasking (PIT IRQ0),
  ring-3 user tasks with per-task address spaces, FPU/SSE state and
  FS-base (TLS) switched per task, `ps`; application processors are
  parked by the bootloader's MP handoff (their `goto_address` is a
  trivial cli/hlt loop) - TUS currently runs on the BSP
- **musl 1.2.6 userspace C library** - ported to the TUS syscall ABI
  (`int $0x80` bridge in `arch/x86_64/syscall_arch.h` +
  `src/internal/tus_syscall.c`). Real C programs (printf, malloc,
  fopen, …) link statically against `musl-out/usr/lib/libc.a`;
  demo at `/boot/musl_hello.elf`
- **ANSI/VT100 console** - the framebuffer text console understands
  the escape sequences a full-screen app needs: cursor positioning,
  erase, 16-colour SGR (+reverse video), cursor show/hide,
  alternate screen
- **termios** - `/dev/tty0` supports TCGETS/TCSETS/TIOCGWINSZ and
  raw mode (ICANON/ECHO/ICRNL honoured); arrow/function keys arrive
  as real escape sequences
- **kilo** - the single-file text editor runs unmodified as a ring-3
  musl program (`exec /boot/kilo.elf <file>`): type, Ctrl-S to save,
  Ctrl-Q to quit; console input ownership lets the shell and a
  foreground app share the keyboard safely
- **tsh** - interactive shell: `help`, `echo`, `clear`, `ver`, `about`,
  `sysinfo`, `reboot`, `crash`, `ls`, `cat`, `mkdir`, `touch`, `rm`,
  `uptime`, `sleep`, `fbfill`, `cd`, `pwd`, `ps`, `exec` (with args)
- **ELF loader** - runs static (ET_EXEC) x86-64 binaries via
  `tsh`'s `exec` command; demo programs at `/boot/hello.elf`,
  `/boot/enforce.elf`, `/boot/musl_hello.elf`, `/boot/kilo.elf`
  (all shipped inside `rootfs.img`)
- **Interrupt handling** - full IDT, remapped PIC, register dump on CPU
  exceptions (kernel panic, incl. CR2 on page faults)

## Project layout

```
TOS/
├── limine-bin/            bootloader binaries (provided)
├── include/limine.h       Limine boot protocol header
├── sources/
│   ├── musl-1.2.6/        userspace C library (ported; source committed)
│   └── kilo/              the kilo text editor (unmodified upstream)
├── musl-out/              musl build output: headers, libc.a, crt (ignored)
├── rootfs/                root filesystem staging dir
│   ├── logo.ppm           boot splash logo (PPM, read at runtime)
│   └── boot/              user programs (built here, into rootfs.img)
├── kernel/
│   ├── linker.ld          higher-half linker script
│   ├── main.c             entry point, boot sequence
│   ├── boot/              boot splash (toast per CPU + logo decoding)
│   ├── arch/x86_64/       CPU-specific: IDT, PIC, CPUID, port I/O, SSE
│   ├── core/              klib (mem + printf), console, bootinfo
│   ├── drivers/           serial, keyboard, framebuffer (ANSI), PPM, PIT
│   ├── mm/                PMM, VMM, kmalloc
│   ├── elf/               elfload port + TUS exec glue
│   ├── vfs/               VFS tree, fd table, rootfs (tar) mount, devices
│   ├── syscall/           int $0x80 gate and dispatch
│   ├── sched/             round-robin scheduler, FPU/TLS task state
│   └── shell/             tsh, core commands, fs commands
├── tests/                 test_boot.py (31 checks) + demo program sources
├── build/                 compiler output (objects, deps) (ignored)
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

Boot with 4 virtual CPUs to see the splash draw four toasts:

```
make run-smp
```

Click into the QEMU window and type `help`. Try:

```
cat /etc/motd
echo hello world > /tmp/greet
cat /tmp/greet
ls /dev
fbfill 336699
uptime
exec /boot/hello.elf
```

Automated headless test (boots with 4 CPUs, types into the virtual
keyboard, verifies the shell responses and the boot splash):

```
make test
```

## The root filesystem image

`rootfs.img` is a ustar tar archive of the `rootfs/` directory, built
by `make` and shipped inside `tus.iso`. Limine loads it as a module
(`module_path: boot():/boot/rootfs.img` in `limine.conf`) and the
kernel parses it into the VFS at boot (`kernel/vfs/rootfs.c`). To add
a file to the running system, drop it into `rootfs/` and rebuild the
ISO - no kernel changes needed. The boot logo is read from
`/logo.ppm` at boot and decoded with the built-in PPM driver
(`kernel/drivers/ppm.c`, P3 and P6), so the splash image can be
replaced by editing `rootfs/logo.ppm`.

## Building a userspace C program (musl)

The ported musl lives in `musl-1.2.6/` and builds into `musl-out/`
(`make musl`). Link your program statically against it, exactly like
`tests/musl_hello.c` in the Makefile (the target places the binary in
`rootfs/boot/` so it ships in the ISO):

```
# compile against the musl headers
gcc -m64 -ffreestanding -fno-stack-protector -fno-pic \
    -mno-red-zone -mgeneral-regs-only -O2 -nostdinc \
    -Imusl-out/usr/include -c hello.c -o hello.o

# link with the musl crt + libc, at the user link address
ld -m elf_x86_64 -static -e _start -Ttext 0x10000000 -o hello.elf \
    musl-out/usr/lib/crt1.o musl-out/usr/lib/crti.o hello.o \
    -Lmusl-out/usr/lib -lc musl-out/usr/lib/crtn.o
```

Then `exec /boot/hello.elf` from tsh.

## Backup policy

Before any significant change, a full backup of the project is taken:

```
tar czf ~/TOS-backup-YYYYMMDD-HHMM.tar.gz /home/quake/TOS
```

## Roadmap

- Userspace: TUS init process, userspace tsh
- More libc syscalls: readdir/getdents, stat, lseek, signals, fork/exec
- Physical disk driver and a real filesystem
- Network stack
- Threads (clone, futex)
