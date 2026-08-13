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

## 4. Kernel state (v0.4.0 — working, 2026-08-13)

v0.4.0 adds **per-task address spaces** and **ring-3 syscall
enforcement**: every user task runs in its own page tables, the
scheduler switches CR3 together with the task, and user callers may
only pass pointers into the user half. Verified by `make test`
(21/21 automated checks).

### 4.0 v0.4.0 components

- **Per-task address spaces** (`kernel/mm/vmm.c`): `vmm_space_clone()`
  allocates a fresh PML4 whose kernel half (indices 256..511) is
  copied from the root space *by reference* — all tasks share the
  kernel image, HHDM, framebuffer and heap. The user half starts
  empty and is private per task, so several tasks can use the same
  link address (the boot test runs two instances of hello.elf, both
  at 0x10000000, with different CR3s). `vmm_space_switch()` changes
  g_cr3 and reloads CR3 (also flushing the TLB); `sched_tick` and
  `task_exit` call it on every task switch.
- **Kernel-half routing**: `vmm_map_page` maps kernel-half addresses
  (>= 0xffff800000000000) in the ROOT tables, user-half addresses in
  the current space. Because the kernel half is shared by reference,
  a kernel mapping is visible from every space. `vmm_reserve_tables`
  pre-creates the heap region's intermediate tables at boot (before
  any task exists), so runtime heap growth only touches shared leaf
  PTEs.
- **ELF loading in a private space** (`kernel/elf/tus_elf.c`):
  `elf_exec` clones a space, switches CR3 to it (preemption
  disabled) so `el_load`'s segment writes land in the right tables,
  spawns the task, and switches back. The task struct stores its
  cr3; `ps` shows it (PID/STATE/CR3/NAME).
- **Ring-3 syscall enforcement** (`kernel/syscall/syscall.c`): the
  syscall stub reads the caller's CS from the interrupt frame and
  passes it to the dispatcher; user callers are limited to canonical
  user-half pointers (`access_ok`), anything else returns -EFAULT
  (-14). The kernel shell (ring 0) is exempt. Proven by the embedded
  `enforce.elf` test image, which passes a kernel address to write()
  and prints the -14 it gets back.
- **cd / pwd** (`kernel/shell/cmd_fs.c`): tsh has a real working
  directory. `cd` validates the target with open + readdir (only
  real directories are entered), `cd` alone returns to `/`;
  relative paths in ls/cat/echo>/mkdir/touch/rm/exec resolve against
  the cwd with `.` and `..` support, and the prompt shows it
  (`tus:/tmp> `). The VFS stays absolute-only; the shell does the
  resolution before calling the syscall ABI.

### 4.1 Earlier phases (v0.2.0 / v0.3.0, archived)

v0.2.0 added the Phase 2 milestone: memory management, timing, the
virtual file system with device nodes, and a POSIX-style syscall ABI
that tsh itself uses. v0.3.0 added the round-robin scheduler and
ring-3 user tasks. All of it is verified by `make test` (21/21
automated checks).

### 4.1a v0.2.0 components

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
- **ELF loader** (`kernel/elf/`): port of the small `elfload`
  library. Loads **static (ET_EXEC)** x86-64 images only — a binary
  linked with `-static` — reading from the VFS (`vfs_pread`) and
  mapping each PT_LOAD segment with the VMM. `tsh`'s `exec` command
  runs the image and returns to the shell when it returns. An
  embedded test program (`tests/hello.elf`) is exposed at
  `/boot/hello.elf`; it calls the real syscall ABI and prints to
  stdout. Images currently run on the kernel stack in ring 0 until
  the scheduler milestone gives them an address space and a ring-3
  environment.
- **kmalloc large blocks**: allocations larger than one page are now
  served from whole arena pages (header records the page count) so
  the kernel can hold multi-KiB blobs such as ELF images.
- **Terminal scrollback**: completed lines are kept in a 2048-line
  history ring; PageUp/PageDown scroll the view (any new output snaps
  back to live). Cells store a palette index so scrolled-back text
  keeps its exact colors (prompt accent included).

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

## 4b. Kernel state (v0.5.0 — musl userspace libc, 2026-08-13)

v0.5.0 ports **musl 1.2.6** as the userspace C library. Userspace
programs can now be written in real C (printf, malloc, fopen, …) and
linked statically against `libc.a`; the first such program
(`tests/musl_hello.elf`) runs as a ring-3 task and is verified by
`make test` (25/25).

### 4b.1 Kernel side

- **New syscalls** (ABI extended, old numbers untouched):
  `SYS_MMAP 12` (anonymous only, zeroed pages, per-task cursor
  starting at 0x40000000), `SYS_MUNMAP 13` (unmaps + returns frames
  to the PMM), `SYS_ARCH_PRCTL 14` (ARCH_SET_FS/ARCH_GET_FS — the
  thread pointer / TLS base), `SYS_WRITEV 15` (scatter write;
  musl's stdio writes through writev, not write).
- **SSE/FPU for user mode**: `cpu_enable_sse()` clears CR0.EM and
  sets CR4.OSFXSR|OSXMMEXCPT (user SSE would #UD otherwise). The
  kernel stays `-mgeneral-regs-only`; instead every task now carries
  a 512-byte fxsave image (`struct task.fpu`) and the scheduler
  fxsaves the outgoing / fxrstors the incoming task on every switch
  (both in `sched_tick` and `task_exit`). Fresh tasks get a default
  FPU state (x87 CW=0x037F, FTW=0xFFFF, MXCSR=0x1F80 — a zeroed
  MXCSR would unmask exceptions).
- **Per-task FS base**: `arch_prctl(ARCH_SET_FS)` writes
  IA32_FS_BASE immediately and stores the value in `task.fs_base`;
  the scheduler reloads the MSR on every task switch, so the C
  library's TLS (errno, stdio locks) survives preemption.
- **Initial user stack**: `task_create_user` now lays out a real
  process image on the (zeroed) user stack:
  `[argc=0][argv[0]=NULL][envp[0]=NULL][auxv: AT_PAGESZ, 4096, 0]`.
  musl's crt1 reads argc at (%rsp) and the C library reads the page
  size from the auxv — without AT_PAGESZ the allocator breaks
  (page_size 0).
- **ELF loader**: `tus_alloc` maps fresh frames; elfload itself
  zeroes BSS (`memset(dest+filesz, 0, memsz-filesz)`), so musl's
  global state (stdin/stdout FILEs) starts clean.

### 4b.2 musl side (musl-1.2.6/ is part of the repo)

- **`arch/x86_64/syscall_arch.h`**: `__syscall0..6` now forward to
  `tus_syscall()` instead of the Linux `syscall` instruction.
- **`src/internal/tus_syscall.c`** (new): the ABI bridge. Translates
  the Linux x86_64 syscall numbers used by the musl source to TUS
  numbers, drops the dirfd of openat/mkdirat/unlinkat, emulates
  nanosleep/clock_nanosleep in userspace (timespec → whole seconds →
  TUS sleep), ignores madvise/poll/umask, and returns -ENOSYS for
  everything else (exactly like an unknown Linux syscall).
- **`src/thread/x86_64/__set_thread_area.s`**: upstream uses the raw
  `syscall` instruction with Linux number 158 — rewritten as
  `int $0x80` with TUS number 14 (same ARCH_SET_FS op).
- **`src/thread/x86_64/syscall_cp.s`**: forwards to `tus_syscall`
  (TUS has no thread cancellation yet; the cancel-flag check stays).
- **Build**: `make musl` (or the first kernel build) runs
  `./configure --target=x86_64-unknown-tus --disable-shared` +
  `make` + `make install DESTDIR=musl-out`. Two quirks: `CC=gcc`
  must be passed to configure (it cross-detects from the target
  tuple), and `AR=ar RANLIB=ranlib` to make (it wants
  `x86_64-unknown-tus-ar`). The build artifacts (obj/, lib/,
  config.mak, musl-out/) are gitignored; the patched source is
  committed.
- **Link recipe** (see Makefile): compile with `-nostdinc
  -Imusl-out/usr/include`, link statically at 0x10000000 with
  `crt1.o crti.o <program> -lc crtn.o`. Entry is crt1's `_start`.

Verified session (serial log):

```
> exec /boot/musl_hello.elf
musl 1.2.6 on TUS: hello from libc
argc=0 argv0=(null)
pid=2
malloc: heap string (128 bytes, strlen=11)
free ok
all good
```

Not yet implemented (return -ENOSYS): fork/exec (process model is
scheduler-based), signals, sockets, readdir/getdents, stat, file
seeking, time-of-day clocks, threads (clone).

## 4c. Kernel state (v0.6.0 — kilo: a real terminal app, 2026-08-13)

v0.6.0 ports **kilo** (antirez's single-file text editor, ~1100 lines
of plain C + libc) as a ring-3 musl program. **Zero changes to
kilo's source** — everything it needs was added to the kernel and the
musl ABI bridge. The editor runs full-screen on the framebuffer
console: raw-mode input, escape-sequence keys, ANSI/VT100 output, and
file save via ftruncate+write. `make test` now verifies the whole
flow (29/29): open a file, type text, Ctrl-S, Ctrl-Q, read it back.

### 4c.1 Kernel side

- **ANSI/VT100 output engine** (kernel/drivers/fb.c): a CSI state
  machine inside `fb_putchar` covering everything a full-screen app
  emits: CUP (`H`/`f`, 1-based), relative moves (`A`/`B`/`C`/`D`),
  erase display/line/chars (`J`/`K`/`X`), SGR colours (`m`: 0
  reset, 7 reverse, 30-37/90-97 fg, 40-47/100-107 bg, 39/49
  default — mapped to the 16-colour VGA palette), cursor visibility
  (`?25h`/`?25l`) and the alternate screen (`?1049h/l`, `?47h/l`
  with a full text-buffer save/restore). Unknown sequences are
  consumed silently. Reverse video is stored as a swapped palette
  index at write time, so redraws reproduce it without per-cell
  flags.
- **Termios on /dev/tty0**: `TCGETS/TCSETS/TCSETSW/TCSETSF`
  (0x5401-0x5404) and `TIOCGWINSZ` (0x5413) via the tty ioctl. The
  kernel stores the 57-byte musl x86_64 `struct termios` blob and
  honours three flags: **ICANON** (off = raw: ESC arrives as a byte,
  Enter stays `\r`), **ICRNL** (the keyboard produces `\n` for
  Enter; raw mode converts it to `\r` so kilo sees a real CR) and
  **ECHO** (canonical programs get typed input echoed; tsh echoes
  itself, so no double echo). Legacy ESC=EOF for `cat` is kept in
  canonical mode. isatty() = TCGETS success now works.
- **Special keys** (keyboard driver): extended scancodes (E0 prefix)
  become `KBD_EVENT_SPECIAL` events (arrows, Home/End, Ins/Del,
  PgUp/PgDn) instead of being dropped; PageUp/PageDown are no longer
  scrollback events (tsh translates them). The tty translates them
  to the escape sequences a real terminal sends (`ESC[A`… `ESC[3~`,
  `ESC[5~`, `ESC[6~`), with a pending-byte buffer so read() returns
  one byte at a time.
- **Console input ownership** (kernel/drivers/keyboard.c): the shell
  and a foreground user task would race for keypresses (both block
  in hlt-waiting loops). `kbd_get_event_owned(pid)` claims the
  console on first tty read (owner pid); the shell uses
  `kbd_get_event_shell(pid)` which never claims — it consumes only
  while the console is free or owned by it. The owner check runs on
  every wake, so the handover (shell releases in cmd_exec before
  spawning; task_exit releases on exit) is race-free.
- **argv forwarding**: `exec <elf> [args...]` passes args through
  elf_exec → task_create_user, which lays out the standard SysV
  image (argc/argv pointers/terminator/envp/auxv) and copies the
  strings at the BOTTOM of the top user-stack page — next to the
  initial RSP they would be clobbered by the first function calls.
  argv[0] = program path (musl_hello now reports argc=1).
- **New syscalls**: `SYS_TIME 16` (seconds since boot, for musl
  `time()`), `SYS_FTRUNCATE 17` (grow/shrink a file with zero-fill
  — kilo saves via truncate+write).

### 4c.2 musl side

- `tus_syscall.c` additions: `ftruncate` (77) and `time` (201) map
  to the new numbers; **`clock_gettime`/`clock_gettime64` (228/403)
  and `gettimeofday` (96) are emulated in userspace** from SYS_TIME.
  This matters more than it looks: musl's `time()` is built on
  `clock_gettime(CLOCK_REALTIME)`, and with ENOSYS it returned
  uninitialised stack garbage — kilo's status-message row was never
  drawn. REALTIME and MONOTONIC both map to the boot clock.

### 4c.3 Verified flow (make test, 29/29)

```
> exec /boot/kilo.elf /kilo.txt
(kilo draws: tildes, welcome, status bar " /kilo.txt - 0 lines")
(typing "hello" -> " /kilo.txt - 1 lines (modified)")
(Ctrl-S -> "5 bytes written on disk")
(Ctrl-Q -> back to shell)
> cat /kilo.txt
hello
```

### 4c.4 Sources moved

musl-1.2.6/ and kilo/ now live under **sources/** (sources/musl-1.2.6,
sources/kilo). Makefile paths updated; kilo's own .git was removed.

## 4d. Kernel state (v0.7.0 — rootfs + boot splash, 2026-08-13)

### 4d.1 Cleaned-up layout, build/ directory

- All compiler output moved to **build/** (`build/kernel/*.o`, `*.d`);
  the source tree holds only sources. Stale `.o`/`.d`/blob artifacts
  removed; screenshots moved to docs/.

### 4d.2 rootfs.img — the initial ram filesystem

- **`rootfs.img`** (ustar tar of the `rootfs/` staging dir) is built by
  `make` and shipped inside `tus.iso` as a **Limine module**
  (`module_path: boot():/boot/rootfs.img`). The kernel mounts it into
  the VFS at boot (`kernel/vfs/rootfs.c` parses the tar; files land at
  `/boot/kilo.elf`, `/logo.ppm`, …).
- **The whole directory tree comes from the image**: `/dev`, `/tmp`,
  `/etc` and `/boot` are tar entries (empty dirs are created by the
  Makefile before packing; git does not track empty dirs), and
  `/etc/motd` is a real file in rootfs/. The kernel does **not**
  hardcode the base directories - `vfs_init()` only creates the root
  node, the rootfs mount provides the tree, and `vfs_devices_init()`
  registers the device nodes afterwards (with a tiny ensure-dirs
  fallback that only fires when the module is missing, keeping the
  serial-only debug path alive).
- The embedded-blob mechanism (`*_blob.o` → `elf_install_test_program`)
  is **gone**; user ELFs are built straight into `rootfs/boot/`.
- Adding a file to the running system = drop it into `rootfs/` and
  rebuild the ISO. No kernel changes.
- **Tar quirk**: directory entries carry a trailing slash (`boot/`);
  the parser strips it before creating the node.

### 4d.3 Boot splash — one toast per CPU

- New **Limine MP request** (`limine_mp_request`): the bootloader
  reports `cpu_count` (4 with `-smp 4`). TUS stays single-CPU: every
  AP is parked with a `cli; hlt` loop published to its `goto_address`
  (`park_aps()` in main.c) — no spin waste in TCG.
- `kernel/boot/splash.c` reads **`/logo.ppm` from the rootfs**, decodes
  it with the new **PPM driver** (`kernel/drivers/ppm.c`, P3 + P6,
  any maxval — no build-time arrays), scales it (16.16 fixed point,
  ≤80% width / ≤45% height, never upscaled) and draws **one toast per
  CPU** centered across the top. The boot log prints below the logo
  band (`fb_set_text_top`); after a 2.5 s hold the shell clears the
  screen and takes over.
- Logo: `rootfs/logo.ppm` (181x200 P3, ~15k non-black px per toast;
  the toast art has black padding inside the frame).
- `make run-smp` boots with `-smp 4` to see four toasts.

### 4d.4 Bug fixed: kfree() on multi-page blocks

The multi-page path of `kfree()` read `b->pages` from the header on
**every loop iteration**; the first iteration unmaps the page that
holds the header, so the second read faulted (#PF at boot, CR2 =
header address). Latent since v0.2.0 — nothing ever freed a >4 KiB
block until the splash's 300 KB logo buffer. Fix: read `pages` once
into a local before the loop.

### 4d.5 Verified flow (make test, 31/31)

- `-smp 4`: banner shows `cpu count    : 4`; framebuffer splash shows
  **four** toasts (~52k warm px) during the hold, then clears to the
  shell.
- All v0.6.0 checks still pass (kilo, scrollback, fbfill, panic dump).

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
11. **Scrollback redraws must reproduce colors**: if the text buffer
    stores only characters, a PageUp/PageDown redraw paints every
    line in the *current* color — the orange prompt turns white. Each
    cell therefore stores a palette index (16 color pairs), and
    `fb_set_color` looks up/appends the pair. Verified pixel-identical
    (50/50 rows) after a scroll round-trip.
12. **QEMU screendump timing**: writing consecutive screendumps to the
    *same* file races — the second read may see the previous frame
    (QEMU truncates/writes asynchronously). Use distinct filenames
    and wait for the file to be non-empty before reading.
13. **kprintf goes to the framebuffer too**: debug prints inside the
    console/scrollback code add lines to the live screen and history,
    skewing pixel-count tests. Remove debug prints before testing
    screen content.
14. **Kernel-half mappings must go to the root space, and the heap's
    page tables must be reserved before any task exists.** With
    per-task spaces, a table allocation (or a 1 GiB large-page split)
    performed in a task's space updates that task's PML4 only —
    other tasks keep the old entry and silently miss the mapping.
    Routing kernel-half maps to the root tables + reserving the heap
    tables at boot (vmm_reserve_tables) means runtime heap growth
    only writes shared leaf PTEs, visible to every space.
15. **Debug prints in the PIT tick handler can starve ring-3 tasks**:
    kprintf of a switch line takes longer than the PIT period in TCG,
    so a new PIT pulse goes pending while IF=0; the iretq into the
    freshly-switched task immediately delivers the pending IRQ0
    before the first user instruction — the task's saved RIP never
    moves past the entry point. Symptom: task "runs" (ticks switch
    to it) but never executes anything. Remove the debug prints.
16. **The syscall stub only returns RAX; all other registers are
    clobbered** (see lesson 7). User programs must declare every
    argument register "+r" in the asm, or GCC assumes they are
    preserved and the compiled code silently uses kernel-garbage
    values after the call (enforce.elf's first version computed a
    buffer pointer from the clobbered RDI and wrote to a garbage
    address → #PF in ring 3).
17. **The caller's CS lives at a fixed offset in the syscall frame**
    (%rsp+64 after the 7 register pushes, for both ring-3 and ring-0
    callers) — do NOT copy it into the register struct: offset 56 is
    the CPU-pushed RIP slot, and writing there makes iretq jump to
    the CS value (0x8) → #PF at address 8. Pass it as a separate
    argument instead.
18. **musl needs the AT_PAGESZ auxv entry**: `libc.page_size` is read
    from the auxiliary vector at startup; with no auxv (or no
    PAGESZ) the allocator sees page_size 0 and breaks. The task
    creation code lays out argc/argv/envp/auxv on the user stack.
19. **musl's TLS init uses the raw `syscall` instruction** in
    `__set_thread_area.s` (it does NOT go through `__syscall()`),
    so the arch asm files must be patched too, not just the C
    wrappers. Same for `syscall_cp.s`.
20. **SSE must be enabled before user programs run** (CR4.OSFXSR),
    and the scheduler must save/restore FPU state per task — musl's
    x86_64 string/math asm is SSE2. Without fxsave/fxrstor on task
    switch, one task's XMM registers leak into the next and
    strlen/memcpy silently corrupt memory.
21. **musl stdio writes via writev, not write** (`__stdio_write`);
    fopen uses openat(257)/mkdirat(258)/unlinkat(263) with
    AT_FDCWD, not open/mkdir/unlink. The remap layer must cover the
    *at variants (dropping dirfd) or every fopen/printf fails.
22. **musl configure/make quirks**: configure cross-detects a
    compiler from `--target` (pass `CC=gcc`) and make wants
    target-prefixed tools (pass `AR=ar RANLIB=ranlib`).
23. **A user task's initial FPU state must be a valid default**
    (MXCSR=0x1F80): fxrstor of a fully zeroed image unmask s all
    SSE exceptions — any NaN in user code then raises #XM.
24. **musl's `time()` is built on clock_gettime(CLOCK_REALTIME),
    not the `time` syscall.** Mapping Linux 201 (time) alone is not
    enough: clock_gettime(228)/clock_gettime64(403)/gettimeofday(96)
    must be emulated too, or time() returns uninitialised stack
    garbage (kilo's status message row was silently blank).
25. **kilo's Enter key is `\r` in raw mode**: the keyboard driver
    produces `\n`; with ICRNL cleared the tty must convert `\n` →
    `\r` so the program sees a real CR (kilo's ENTER).
26. **The initial argv strings must sit at the BOTTOM of the user
    stack page**, not just below the pointer array: the first ~4 KiB
    of stack usage (function calls, musl startup) would clobber
    them. RSP starts at the array; strings live ~4 KiB away.
27. **Console input needs ownership arbitration**: with two hlt-
    waiting consumers (shell + foreground task) keypresses split
    between them nondeterministically. The owner check must run
    INSIDE the wait loop (re-checked on every wake), and the shell
    must never claim — otherwise it re-claims before the new task
    gets its first read and eats all keys.
28. **`struct kbd_event` layout changed**: adding a `code` field
    broke `{ KBD_EVENT_CHAR, c }` initializers (c landed in `code`,
    `c` stayed 0 — keys silently ignored). Designated initializers
    or a zero-fill are mandatory once a struct has more than one
    meaning-bearing field.
29. **Rare #PF in fb_scroll_up's memset (dest ≈ 0x10001054/0x3, len
    0x23/0x10000070) — ROOT CAUSED AND FIXED**: the round-robin
    switch stubs (sched_tick_entry, task_exit) saved only the 9
    caller-saved registers. When the PIT tick preempted ring-0 code
    mid-function (fb_putchar's inlined scroll) and detoured through
    a ring-3 task, the user code clobbered rbx/rbp/r12-r15 — and on
    return the interrupted kernel code continued with the USER task's
    register values, computing a garbage framebuffer destination
    (memset into an unmapped user address → #PF). It never showed
    before because user tasks only ever interrupted code that didn't
    depend on callee-saved registers (hlt loops, syscall stubs).
    FIX: FRAME_WORDS 14→20, the stubs push/pop rbx rbp r12-r15, and
    fresh-task frames zero them. The #PF handler now also saves ALL
    registers + 24 stack words, which is what cracked this.
30. **QEMU/process hygiene**: `pkill -f` patterns match your own
    shell command line — run pkill in a separate exec step, and
    delete stale serial/QMP/socket files between runs or a watcher
    reads the previous run's log.
24. **The initial user stack must be zeroed**: pmm frames are not
    zeroed on allocation. musl's crt1 walks argc/argv and the C
    library scans envp/auxv; garbage there means garbage argc or a
    wild auxv walk (crash before main).

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
| 12 | ELF loader: run static (ET_EXEC) images (`exec` in tsh) | ✅ done (v0.2.1) |
| 13 | Scheduler: round-robin preemptive multitasking | ✅ done (v0.3.0) |
| 14 | Ring 3: user-mode ELF tasks (exec, ps) | ✅ done (v0.3.0) |
| 16 | Per-task address spaces + syscall ring-3 enforcement | ✅ done (v0.4.0) |
| 17 | **musl 1.2.6 userspace libc** (printf/malloc/stdio/TLS via int $0x80 bridge) | ✅ done (v0.5.0) |
| 15 | Userspace: init, userspace tsh | ⏳ |
| 15 | Physical disk driver + real filesystem | ⏳ |
| ... | (expand as we go) | |

---

*"Work everywhere, but work right."* — TUS
