/*
 * syscall.h - POSIX-style system call interface
 *
 * Syscalls are invoked with `int $0x80`:
 *
 *     RAX = syscall number
 *     RDI, RSI, RDX, R10, R8 = arguments (up to five)
 *     RAX = return value (>= 0, or negative errno on failure)
 *
 * The IDT gate at vector 0x80 is a DPL-3 trap gate, so the same ABI
 * will be used by user-mode processes once the scheduler exists. Until
 * then the kernel shell calls these directly.
 */

#ifndef TUS_SYSCALL_SYSCALL_H
#define TUS_SYSCALL_SYSCALL_H

#include <stdint.h>

/* Syscall numbers (documented ABI; keep stable). */
#define SYS_EXIT    0
#define SYS_READ    1
#define SYS_WRITE   2
#define SYS_OPEN    3
#define SYS_CLOSE   4
#define SYS_IOCTL   5
#define SYS_GETPID  6
#define SYS_UPTIME  7
#define SYS_SLEEP   8
#define SYS_MKDIR   9
#define SYS_UNLINK  10
#define SYS_READDIR 11
/* v0.5.0: userspace libc (musl) support. */
#define SYS_MMAP     12 /* mmap(addr, len, prot, flags) - anonymous only */
#define SYS_MUNMAP   13 /* munmap(addr, len) */
#define SYS_ARCH_PRCTL 14 /* arch_prctl(op, addr) - ARCH_SET_FS/ARCH_GET_FS */
#define SYS_WRITEV   15 /* writev(fd, iovec*, count) */
/* v0.6.0: kilo (a real terminal application) needs a clock and file
 * truncation. */
#define SYS_TIME     16 /* time(NULL): seconds since boot */
#define SYS_FTRUNCATE 17 /* ftruncate(fd, length) */
/* v0.8.0: execve + uid/gid (doas, passwd, login, useradd). */
#define SYS_EXECVE   18 /* execve(path, argv, envp) - replaces the task */
#define SYS_CHMOD    19 /* chmod(path, mode) */
#define SYS_GETUID   20 /* getuid() */
#define SYS_GETEUID  21 /* geteuid() */
#define SYS_SETUID   22 /* setuid(uid) */
#define SYS_GETGID   23 /* getgid() */
#define SYS_SETGID   24 /* setgid(gid) */

/* IDT entry stub (vector 0x80). Installed by idt_init(). */
void syscall_entry(void);

/* Perform a syscall from kernel mode (what tsh uses today).
 *
 * The kernel stub (syscall_entry) uses the argument registers as
 * scratch and never restores them - only RAX comes back. Every
 * argument register is therefore declared read-write ("+"): GCC
 * knows the asm may clobber them and reloads them before every call
 * instead of assuming they survived the previous one. */
static inline long syscall(long number, long a1, long a2, long a3,
                           long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = 0;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    __asm__ volatile("int $0x80"
                     : "=a"(ret), "+r"(rdi), "+r"(rsi), "+r"(rdx),
                       "+r"(r10), "+r"(r8), "+r"(r9)
                     : "0"(number)
                     : "rcx", "r11", "memory");
    return ret;
}

#endif /* TUS_SYSCALL_SYSCALL_H */
