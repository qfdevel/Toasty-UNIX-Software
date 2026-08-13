/*
 * syscall.c - system call dispatch
 *
 * The IDT gate at vector 0x80 enters syscall_entry(), a tiny naked
 * stub that pushes the seven argument registers and calls
 * syscall_dispatch() with a pointer to them. The dispatch table maps
 * POSIX-style numbers onto the VFS, timer and process APIs.
 *
 * Ring-3 enforcement: the stub also records the caller's CS (pushed
 * by the CPU as part of the interrupt frame), so the dispatcher knows
 * whether the call came from user mode. User callers may only pass
 * pointers into the canonical user half; anything in the kernel half
 * is rejected with -EFAULT. (The kernel shell itself calls the same
 * ABI from ring 0 and is exempt.)
 */

#include "syscall.h"

#include <stdbool.h>

#include "../arch/x86_64/io.h"
#include "../core/errno.h"
#include "../drivers/pit.h"
#include "../sched/sched.h"
#include "../vfs/vfs.h"

/* Register image as pushed by syscall_entry() (lowest address first).
 * The caller's CS is passed as a separate argument to the dispatcher
 * (it lives in the CPU-pushed frame, not among the registers). */
struct syscall_regs {
    uint64_t rax; /* syscall number */
    uint64_t rdi; /* arg 1 */
    uint64_t rsi; /* arg 2 */
    uint64_t rdx; /* arg 3 */
    uint64_t r10; /* arg 4 */
    uint64_t r8;  /* arg 5 */
    uint64_t r9;  /* arg 6 (unused for now) */
};

/* Vector 0x80 gate: save the registers, dispatch, restore, iretq.
 * After the seven pushes, the CPU-pushed frame starts at %rsp+56:
 * RIP at +56, CS at +64 (valid for both ring-3 and ring-0 callers).
 * CS is handed to the dispatcher in %rsi. */
__attribute__((naked)) void syscall_entry(void) {
    __asm__ volatile(
        "push %r9\n\t"
        "push %r8\n\t"
        "push %r10\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %rax\n\t"
        "mov %rsp, %rdi\n\t"
        "mov 64(%rsp), %rsi\n\t"   /* CS from the interrupt frame */
        "call syscall_dispatch\n\t"
        "add $56, %rsp\n\t"
        "iretq\n");
}

/* Upper bound of the canonical user half (0x00007fffffffffff). The
 * kernel half starts at 0xffff800000000000; non-canonical addresses
 * sit in between and are rejected too. */
#define USER_HALF_MAX 0x00007fffffffffffull

/* True when a user-mode caller may reference [ptr, ptr+len). Ring-0
 * callers (the shell) pass kernel pointers freely. */
static bool access_ok(bool from_user, const void *ptr, size_t len) {
    if (!from_user) {
        return true;
    }
    uint64_t a = (uint64_t)(uintptr_t)ptr;
    uint64_t e = a + len;
    return e >= a && e <= USER_HALF_MAX;
}

/* exit(status): terminate the current task and switch to the next.
 * Never returns. */
__attribute__((noreturn)) static long sys_exit(int status) {
    task_exit(status);
}

long syscall_dispatch(struct syscall_regs *r, uint64_t cs) {
    bool from_user = (cs & 3) == 3;

    switch (r->rax) {
    case SYS_EXIT:
        return sys_exit((int)r->rdi);
    case SYS_READ:
        if (!access_ok(from_user, (void *)r->rsi, (size_t)r->rdx)) {
            return -EFAULT;
        }
        return vfs_read((int)r->rdi, (void *)r->rsi, (size_t)r->rdx);
    case SYS_WRITE:
        if (!access_ok(from_user, (const void *)r->rsi, (size_t)r->rdx)) {
            return -EFAULT;
        }
        return vfs_write((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx);
    case SYS_OPEN:
        if (!access_ok(from_user, (const void *)r->rdi, 1)) {
            return -EFAULT;
        }
        return vfs_open((const char *)r->rdi, (int)r->rsi);
    case SYS_CLOSE:
        return vfs_close((int)r->rdi);
    case SYS_IOCTL:
        if (!access_ok(from_user, (void *)r->rdx, 1)) {
            return -EFAULT;
        }
        return vfs_ioctl((int)r->rdi, r->rsi, (void *)r->rdx);
    case SYS_GETPID: {
        struct task *cur = sched_current();
        return cur != NULL ? (long)cur->pid : 1;
    }
    case SYS_UPTIME:
        return (long)pit_uptime_ms();
    case SYS_SLEEP:
        timer_sleep_ms((uint32_t)r->rdi);
        return 0;
    case SYS_MKDIR:
        if (!access_ok(from_user, (const void *)r->rdi, 1)) {
            return -EFAULT;
        }
        return vfs_mkdir((const char *)r->rdi);
    case SYS_UNLINK:
        if (!access_ok(from_user, (const void *)r->rdi, 1)) {
            return -EFAULT;
        }
        return vfs_unlink((const char *)r->rdi);
    case SYS_READDIR:
        if (!access_ok(from_user, (void *)r->rsi, (size_t)r->rdx)) {
            return -EFAULT;
        }
        return vfs_readdir((int)r->rdi, (void *)r->rsi, (size_t)r->rdx);
    default:
        return -ENOSYS;
    }
}
