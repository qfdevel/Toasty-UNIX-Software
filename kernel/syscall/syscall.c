/*
 * syscall.c - system call dispatch
 *
 * The IDT gate at vector 0x80 enters syscall_entry(), a tiny naked
 * stub that pushes the seven argument registers and calls
 * syscall_dispatch() with a pointer to them. The dispatch table maps
 * POSIX-style numbers onto the VFS, timer and process APIs.
 */

#include "syscall.h"

#include "../arch/x86_64/io.h"
#include "../core/console.h"
#include "../core/errno.h"
#include "../drivers/pit.h"
#include "../sched/sched.h"
#include "../vfs/vfs.h"

/* Register image as pushed by syscall_entry() (lowest address first). */
struct syscall_regs {
    uint64_t rax; /* syscall number */
    uint64_t rdi; /* arg 1 */
    uint64_t rsi; /* arg 2 */
    uint64_t rdx; /* arg 3 */
    uint64_t r10; /* arg 4 */
    uint64_t r8;  /* arg 5 */
    uint64_t r9;  /* arg 6 (unused for now) */
};

/* Vector 0x80 gate: save the registers, dispatch, restore, iretq. */
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
        "call syscall_dispatch\n\t"
        "add $56, %rsp\n\t"
        "iretq\n");
}

/* exit(status): terminate the current task and switch to the next.
 * Never returns. */
__attribute__((noreturn)) static long sys_exit(int status) {
    task_exit(status);
}

long syscall_dispatch(struct syscall_regs *r) {
    switch (r->rax) {
    case SYS_EXIT:
        return sys_exit((int)r->rdi);
    case SYS_READ:
        return vfs_read((int)r->rdi, (void *)r->rsi, (size_t)r->rdx);
    case SYS_WRITE:
        return vfs_write((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx);
    case SYS_OPEN:
        return vfs_open((const char *)r->rdi, (int)r->rsi);
    case SYS_CLOSE:
        return vfs_close((int)r->rdi);
    case SYS_IOCTL:
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
        return vfs_mkdir((const char *)r->rdi);
    case SYS_UNLINK:
        return vfs_unlink((const char *)r->rdi);
    case SYS_READDIR:
        return vfs_readdir((int)r->rdi, (void *)r->rsi, (size_t)r->rdx);
    default:
        return -ENOSYS;
    }
}
