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

### 3.2 `limine.conf` (user-approved format)

```
TIMEOUT=0

:Toasty Unix Software (TUS)
    PROTOCOL=limine
    KERNEL_PATH=boot:///kernel.elf
```

Notes:

- `TIMEOUT=0` → boots immediately, no menu wait.
- Entry name: `Toasty Unix Software (TUS)`.
- `PROTOCOL=limine` → the native Limine boot protocol (not multiboot2).
- `KERNEL_PATH=boot:///kernel.elf` → kernel lives at ISO root `/boot/kernel.elf`.

## 4. Toolchain Status (verified 2026-08-12)

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

## 9. Roadmap / Status

| Phase | Item | Status |
|-------|------|--------|
| 0 | Spec & notes saved (TUS.md) | ✅ done |
| 1 | Repo scaffold: Makefile, linker, entry.asm, limine.conf, ISO script | ⏳ pending — start on user command |
| 2 | Boot: Limine → long mode → hello world (serial + framebuffer) | ⏳ |
| 3 | `/dev/fb0` framebuffer device (write + draw) | ⏳ |
| 4 | GDT/IDT, interrupts, PIT timer | ⏳ |
| 5 | Memory: physical allocator + paging (higher half) | ⏳ |
| 6 | Serial/kbd drivers, minimal VFS, syscalls | ⏳ |
| 7 | Userspace: TUS shell, init process | ⏳ |
| ... | (expand as we go) | |

---

*"Work everywhere, but work right."* — TUS
