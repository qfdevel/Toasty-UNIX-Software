# TUS — Toasty Unix Software

**Project specification & development notes**

---

## 1. Identity

| Field     | Value                     |
|-----------|---------------------------|
| Full name | Toasty Unix Software      |
| Acronym   | TUS                       |
| Motto     | *"Work everywhere, but work right."* |
| Type      | UNIX-like operating system |
| Architecture | AMD64 (x86-64, 64-bit long mode) |
| Goal      | Run on **all** 64-bit systems — modular, clean, well-organized source |

## 2. Core Requirements

- **Full operating system**, UNIX-like in design and behavior.
- **AMD64 / x86-64 only** (64-bit long mode). No 32-bit legacy mode code paths.
- **Framebuffer as a device**: exposed as `/dev/fb0`, the standard UNIX way.
- **Portability**: must run on all 64-bit machines → keep hardware abstraction
  clean, isolate drivers behind a small, well-defined kernel API.
- **Modular, spotless source code**: every file has a purpose, every function
  is documented, naming and comments in **English only**.
- **Professional quality code** — this is a project to be proud of.

## 3. Bootloader: Limine

We use the **Limine bootloader** (v12.5.2 installed) because:

- It switches the CPU directly into **64-bit long mode** before the kernel runs
  — no real/protected-mode bootstrap code of our own needed.
- It hands us a **protected, ready-to-use framebuffer address** (via the
  Limine boot protocol) — no manual VBE mode hunting.
- Mature, BSD-2-Clause licensed, supports BIOS + UEFI from one image.

### 3.1 Kernel entry contract (Limine boot protocol)

- Kernel is a **freestanding ELF64** file (`kernel.elf`) linked for long mode.
- The kernel defines **limine requests** (static structs in a special section)
  that Limine fills in before jumping to the entry point.
- Required request for the framebuffer: `limine_framebuffer_request` →
  after boot, `request.response->framebuffers[]` gives us:
  - `address` (physical framebuffer base), `width`, `height`, `pitch`,
    `bpp`, `memory_model` (usually RGB, 32 bpp).
- Entry point receives `(uint64_t magic, uint64_t limine_bootloader_address)`
  as its first two arguments (per Limine C calling convention).
- Limine maps the framebuffer into the higher-half kernel space for us.

### 3.2 `limine.conf` (WORKING FORMAT — Limine 12.x)

```
timeout: 0

# Mirror the boot log and menu to the serial port (COM1) for debugging.
SERIAL: yes

/Toasty Unix Software (TUS)
    protocol: limine
    kernel_path: boot():/boot/kernel.elf
```

> **IMPORTANT**: The syntax shown in the original request
> (`TIMEOUT=0`, `PROTOCOL=limine`, `KERNEL_PATH=boot:///kernel.elf`,
> entry starting with `:`) is Limine **5.x** style and is **rejected by
> Limine 12.x** (`[config file contains no valid entries]`). The 12.x
> equivalents are: `timeout: 0`, entry names starting with **`/`**,
> `protocol: limine`, `kernel_path: boot():/boot/kernel.elf`.

### 3.3 Kernel entry state (from the Limine boot protocol spec)

- The kernel is entered via an `iretq` frame with **CS = 0x28** and
  **DS/ES/SS/FS/GS = 0x30** (flat 64-bit code/data descriptors set up
  by Limine). This is a **guaranteed part of the protocol**.
- **Consequence for the IDT**: every interrupt gate must use selector
  **0x28**. Selector 0x08 is Limine's 32-bit compat segment — using it
  makes the CPU execute handlers as 32-bit code → #GP → double fault →
  triple fault (instant CPU reset, looks like the VM "shut down").
- Limine maps the kernel ELF at its linked higher-half addresses, sets
  up a stack (≥16 KiB), and fills in every `.requests` struct.

## 4. Kernel state (v0.2.0 — working, 2026-08-13)

v0.2.0 adds the Phase 2 milestone: memory management, timing, the
virtual file system with device nodes, and a POSIX-style syscall ABI
that tsh itself uses. All of it is verified by `make test` (16/16
automated checks).

### 4.1 Phase 2 components

- **PMM** (`kernel/mm/pmm.c`): bitmap frame allocator over the Limine
  memory map. Only `LIMINE_MEMMAP_USABLE` frames are allocated; the
  kernel/modules and framebuffer regions are excluded by the
  bootloader. First-fit with a hint, `pmm_alloc_frame(s)`, free,
  phys→virt, stats. Boot: 130591 frames (510 MiB usable), ~2 used.
- **VMM** (`kernel/mm/vmm.c`): extends Limine's own page tables
  (read CR3, allocate PDPT/PD/PT from the PMM on demand) instead of
  replacing them — safe early design. `vmm_map_page`/`map_region`/
  `unmap_page` + `invlpg`.
- **kmalloc** (`kernel/mm/kmalloc.c`): free-list heap at
  `KHEAP_BASE = 0xffffffff81000000` (64 MiB cap), split/coalesce,
  `krealloc`. The heap grows by mapping fresh frames from the PMM.
- **PIT** (`kernel/drivers/pit.c`): IRQ0 at 100 Hz (divisor 11932,
  mode 3), `pit_uptime_ms()` and `timer_sleep_ms()` (hlt loop). The
  timer API is designed so an APIC timer can replace it later.
- **VFS** (`kernel/vfs/vfs.c`): ramfs tree (`/`, `dev/`, `tmp/`,
  `boot/`, `etc/motd`), fd table (16 slots, 0/1/2 pre-opened on
  `/dev/tty0`), open/read/write/close/ioctl/readdir/mkdir/unlink.
- **Devices** (`kernel/vfs/devices.c`): `/dev/fb0` (raw pixel
  read/write by byte offset + `FB_IOCTL_GET_INFO`/`FB_IOCTL_FILL`),
  `/dev/tty0` (write→console, read→keyboard, ESC=EOF), `/dev/kbd0`,
  `/dev/serial0` (write-only), `/dev/null`, `/dev/zero`.
- **Syscalls** (`kernel/syscall/syscall.c`): `int $0x80` trap gate
  (DPL 3, vector 0x80). Numbers: exit 0, read 1, write 2, open 3,
  close 4, ioctl 5, getpid 6, uptime 7, sleep 8, mkdir 9, unlink 10,
  readdir 11. Errors are negative errno (ENOENT=2, EINVAL=22, …).
  Ring-0 for now; ring-3 enforcement comes with userspace.
- **tsh additions**: `ls`, `cat`, `echo` (with `> file`
  redirection), `mkdir`, `touch`, `rm`, `uptime`, `sleep`,
  `fbfill <hexcolor>`; `sysinfo` now shows PMM stats + uptime. All
  file commands go through the syscall ABI (dogfooding).

Verified session (serial log):

```
tus> cat /etc/motd
Welcome to TUS - Toasty Unix Software.
"Work everywhere, but work right."
tus> echo hello world > /tmp/greet
tus> cat /tmp/greet
hello world
tus> uptime
uptime: 15.920 s
tus> fbfill 336699
fb0: filled with #336699
```

## 4a. Kernel state (v0.1.0 — archived 2026-08-12)

Boots from the ISO in QEMU (BIOS), serial + framebuffer console,
interrupt-driven PS/2 keyboard, and an interactive `tsh`. Verified by
`make test` (10/10 automated checks).

## 5. Hard-won lessons (read before touching the kernel)

1. **IDT selector must be 0x28** (the protocol's CS), never 0x08.
   Wrong selector = triple fault on the very first interrupt.
2. **Registered IRQ handlers are plain C functions.** Only the IDT
   stubs in idt.c carry `__attribute__((interrupt))`. A handler that
   returns with IRETQ instead of RET pops garbage → #GP.
3. **GCC flags for a higher-half kernel**: `-mcmodel=kernel` (avoids
   32-bit relocations against rodata), `-mno-red-zone`,
   `-mgeneral-regs-only`, `-fno-pic`, `-ffreestanding`.
4. **QEMU sendkey timing**: PS/2 buffers are small (16 bytes); typing
   faster than ~100 ms/key drops scancodes (Enter gets lost, words
   concatenate). The automated test uses 30–50 ms gaps only because
   it sends short strings.
5. **limine.conf is 12.x syntax** (see §3.2); `SERIAL: yes` is
   invaluable for debugging (Limine mirrors its log to COM1).
6. **Orphan `.requests` section must be kept** in the linker script
   (`KEEP(*(.requests))`); base revision is found via the symbol
   `limine_base_revision`, declared as
   `static volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(2);`.
7. **syscall wrapper must use GCC register variables (Linux pattern),
   not plain operands.** The kernel stub never restores the argument
   registers (only RAX comes back). With `"D"/"S"/"d"` operands GCC
   assumes the asm preserves RSI/RDX and will NOT reload them between
   calls — the second `read()` in a loop silently gets a garbage
   buffer. Fix: `register long rdi asm("rdi") = a1;` … and pass them
   as read-write (`"+r"`) operands, so GCC knows they are clobbered
   and reloads before every call. Don't push/pop inside the asm
   either: it shifts GCC's `(%rsp)`-based memory operands.
8. **The Makefile does not track header dependencies** (no `-MMD`).
   After editing a header, `make clean` first or you will test a
   stale binary and chase ghosts.
9. **`path_split` must not copy a leading slash into the name**: the
   tree stores `"dev"`, lookup compares `"dev"`. Storing `"/dev"`
   makes `ls /` show `/dev` but `ls /dev` fail with ENOENT.
10. **`devices_init()` must run before `vfs_init` pre-opens fd 0/1/2**
    on `/dev/tty0`, or the standard descriptors stay NULL.

## 6. Toolchain Status (verified 2026-08-12)

All required tools are installed on this Arch Linux machine:

| Tool               | Version     | Status |
|--------------------|-------------|--------|
| gcc                | 16.1.1      | ✅     |
| nasm               | 3.02        | ✅     |
| make               | 4.4.1       | ✅     |
| ld (binutils)      | present     | ✅     |
| qemu-system-x86_64 | 11.0.3      | ✅     |
| limine             | 12.5.2      | ✅     |
| xorriso            | present     | ✅     |
| grub-mkrescue      | present     | ✅ (fallback only) |

Limine boot files available at `/usr/share/limine/`:

- `limine-bios.sys`, `limine-bios-cd.bin` (BIOS boot)
- `limine-uefi-cd.bin`, `BOOTX64.EFI` (UEFI boot)

## 5. Planned ISO Build Flow (to be implemented at start)

1. `make` → freestanding ELF64 `kernel.elf` (higher-half, long mode).
2. Assemble ISO tree:
   - `iso_root/boot/kernel.elf`
   - `iso_root/boot/limine.conf`
   - `iso_root/EFI/BOOT/BOOTX64.EFI` (+ `BOOTIA32.EFI`)
   - `iso_root/boot/limine-bios.sys`
3. `xorriso -as mkisofs` … `-eltorito-alt-boot` … + `limine bios-install` step
   (standard Limine hybrid ISO procedure; script from Limine docs).
4. Boot test: `qemu-system-x86_64 -cdrom tus.iso -m 512M -serial stdio` (BIOS),
   plus a UEFI run via OVMF if available.

## 6. Target Architecture (initial design sketch)

```
arch/x86_64/        — CPU/GDT/IDT/entry, arch-specific glue
kernel/             — core: memory, scheduler, VFS, syscalls
drivers/            — framebuffer, keyboard, serial, timer, disk
dev/                — device node layer → /dev/fb0, /dev/tty0, ...
sys/                — UNIX interfaces: fork/exec/open/read/write...
include/            — public headers (limine.h, tus/...)
```

Design principles:

- **Higher-half kernel** (e.g. `0xffffffff80000000`) — standard for x86-64.
- **UNIX VFS** from day one: framebuffer is a character device at `/dev/fb0`
  supporting `open/read/write/mmap/ioctl`.
- **Modularity**: drivers register against a stable device API; no globals
  leaking across subsystems; clean headers with doxygen-style comments.
- **English everywhere**: identifiers, comments, commit messages, docs.
- Every subsystem gets a `README` or header comment explaining *why* it exists.

## 7. Coding Standards (mandatory)

- Language: C (freestanding, `-ffreestanding -fno-stack-protector -O2`),
  assembly only where required (`entry.asm`, CPU init).
- All code, comments, and documentation in **English**.
- Naming: `snake_case` functions/variables, `SCREAMING_SNAKE` constants,
  `tus_` prefix for public kernel API to avoid symbol collisions.
- Every public function documented: purpose, params, returns, caveats.
- No magic numbers without a named constant or comment.
- Error handling: kernel-style errno (negative return values), no silent
  failure in core paths.
- Format: 4-space indent, braces on next line (Allman) — consistent `clang-format`.

## 8. Backup Policy (ABSOLUTE RULE)

> **Before any significant change: take a backup. Never forget.**

- Before starting the project, before big refactors, before risky experiments:
  `tar czf ~/TOS-backup-<date>-<time>.tar.gz /home/quake/TOS`
- Keep the latest backup in `~/` (outside the project dir).
- Name backups clearly: `TOS-backup-YYYYMMDD-HHMM.tar.gz`.
- If a test/demo makes a mess → restore from backup, never hand-patch chaos.

## 10. Roadmap / Status

| Phase | Item | Status |
|-------|------|--------|
| 0 | Spec & notes saved (TUS.md) | ✅ done |
| 1 | Repo scaffold: Makefile, linker, limine.conf, ISO script | ✅ done |
| 2 | Boot: Limine → long mode → serial + framebuffer console | ✅ done |
| 3 | IDT/PIC, exceptions with register dump | ✅ done |
| 4 | PS/2 keyboard driver (IRQ1, Shift/Caps/Ctrl) | ✅ done |
| 5 | **tsh** — TUS shell with 8 built-in commands | ✅ done |
| 6 | Automated test suite (tests/test_boot.py, 10/10) | ✅ done |
| 7 | Physical memory manager + higher-half paging | ✅ done (v0.2.0) |
| 8 | PIT timer (100 Hz, uptime, sleep) | ✅ done (v0.2.0) |
| 9 | VFS + device nodes (`/dev/fb0`, tty0, kbd0, serial0, null, zero) | ✅ done (v0.2.0) |
| 10 | POSIX syscall ABI (int $0x80, 12 syscalls, dogfooded by tsh) | ✅ done (v0.2.0) |
| 11 | Automated test suite extended (16/16) | ✅ done (v0.2.0) |
| 12 | Userspace: init, userspace tsh | ⏳ |
| 13 | Scheduler + ring-3 enforcement of syscalls | ⏳ |
| 14 | Physical disk driver + real filesystem | ⏳ |
| ... | (expand as we go) | |

---

*"Work everywhere, but work right."* — TUS
