/*
 * sched.h - round-robin task scheduler
 *
 * The scheduler is driven by the PIT (IRQ0, 100 Hz). A tiny assembly
 * stub saves the caller-saved registers plus the interrupt frame on
 * the current task's kernel stack, asks the C core for the next task,
 * and either returns to the same task or switches RSP to the next
 * task's saved frame and IRETQs into it.
 *
 * Task 0 is the kernel shell (tsh) itself, running in ring 0. Tasks
 * created with task_create_user() run in ring 3 via an IRETQ frame
 * that loads the user segments, the user stack and the entry point.
 */

#ifndef TUS_SCHED_SCHED_H
#define TUS_SCHED_SCHED_H

#include <stdint.h>

#include "../vfs/vfs.h" /* VFS_MAX_FDS */

struct vfs_file;

/* Task states. */
#define TASK_READY   1
#define TASK_RUNNING 2
#define TASK_ZOMBIE  3

#define TASK_NAME_MAX 32
#define TASK_MAX      16

struct task {
    uint32_t pid;
    uint32_t state;
    char name[TASK_NAME_MAX];

    /* Kernel stack: RSP0 (top) is what TSS.RSP0 points at; rsp is
     * the saved frame position while the task is not running. */
    uint64_t kstack;
    uint64_t kstack_top;
    uint64_t rsp;

    /* User stack (ring 3 tasks). */
    uint64_t ustack;
    uint64_t ustack_top;

    /* Per-task address space (physical PML4). Tasks created by
     * task_create_user() run in their own space; the kernel half is
     * shared with the root space, the user half is private. */
    uint64_t cr3;

    /* FPU/SSE state (fxsave image, 512 bytes, 16-byte aligned). User
     * programs compiled with SSE use the XMM registers; the kernel
     * itself is compiled with -mgeneral-regs-only and never touches
     * them, but they must survive a task switch, so every switch
     * fxsaves the outgoing task and fxrstors the incoming one. */
    uint8_t fpu[512] __attribute__((aligned(16)));

    /* User-mode FS base (thread pointer / TLS). Written by
     * arch_prctl(ARCH_SET_FS); reloaded into the MSR on every task
     * switch (the C library stores errno and TLS in it). */
    uint64_t fs_base;

    /* User identity. TUS has no privilege separation yet - every task
     * starts as uid/gid 0 (root) - but the ids are tracked and can be
     * queried/changed via the uid/gid syscalls, which is what doas,
     * passwd and login use. */
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
    uint32_t egid;

    /* Next address for anonymous mmap allocations (see SYS_MMAP). */
    uint64_t mmap_cur;

    /* Per-task file descriptor table (open files, see vfs.c). A new
     * task inherits a refcounted copy of its creator's table at
     * spawn (POSIX fd inheritance without fork); task_exit closes
     * every entry. tsh sets up slots 0/1/2 (redirection, pipes)
     * before spawning, which is exactly how `a | b` and `> file`
     * work: the child keeps its copy when the shell restores its
     * own. */
    struct vfs_file *fds[VFS_MAX_FDS];

    int exit_status;
};

/* Initialise the scheduler; the calling context (tsh) becomes task 0. */
void sched_init(void);

/* IRQ0 entry point (assembly stub, see sched.c). */
void sched_tick_entry(void);

/* C core of the tick: called from the stub with RSP pointing at the
 * saved registers. Returns the RSP to switch to, or 0 to continue. */
uint64_t sched_tick(uint64_t frame_rsp);

/* Create a ring-3 task that starts executing `entry` in the address
 * space `cr3` (see vmm_space_clone). The user stack is mapped inside
 * that space, and the fake interrupt frame is pre-built so the first
 * switch IRETQs straight into ring 3. `argc`/`argv` (argv[0] is
 * conventionally the program path) are copied onto the user stack in
 * the standard SysV layout so a C runtime sees real arguments.
 * Returns the new PID or -1. */
int task_create_user(uint64_t entry, const char *name, uint64_t cr3,
                     int argc, char **argv);

/* Current task; NULL before sched_init(). */
struct task *sched_current(void);

/* Terminate the current task (noreturn; switches to the next). */
void task_exit(int status) __attribute__((noreturn));

/* Preemption control: while the counter is non-zero the PIT tick does
 * not switch tasks (used around kernel code that must not be
 * interrupted mid-way, e.g. kprintf's format walk). Nested-safe. */
void preempt_disable(void);
void preempt_enable(void);

/* Number of live tasks (for sysinfo). */
int sched_task_count(void);

/* Print the task table (ps command): PID, state, name. */
void task_list_all(void);

/* True while a task with this pid exists and has not exited. The
 * shell spins on this (hlt) to wait for a spawned pipeline stage. */
int sched_task_alive(uint32_t pid);

#endif /* TUS_SCHED_SCHED_H */
