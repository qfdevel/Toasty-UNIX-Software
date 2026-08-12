/*
 * sched.c - round-robin task scheduler implementation
 *
 * Each task has its own kernel stack. The IRQ0 assembly stub
 * (sched_tick_entry) runs on the current task's kernel stack, pushes
 * the caller-saved registers and calls sched_tick() with RSP as the
 * argument. sched_tick() records that RSP in the task, picks the next
 * ready task, updates TSS.RSP0, and returns the next task's saved RSP
 * (or 0 to stay). The stub then switches RSP and IRETQs, which
 * resumes the other task exactly where it was interrupted.
 *
 * A freshly created user task's kernel stack is pre-filled with a
 * fake interrupt frame so that the first switch IRETQs straight into
 * ring 3: SS=user data, RSP=user stack top, RFLAGS with IF set,
 * CS=user code, RIP=entry point. The task runs until it makes a
 * syscall or is preempted; on SYS_EXIT it calls task_exit(), which
 * switches to the next task and never returns.
 */

#include "sched.h"

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "core/console.h"
#include "core/klib.h"
#include "drivers/pit.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* Selectors from gdt.c (duplicated here to keep sched.c self-contained
 * for the fake IRETQ frame; they must match gdt.c). User segments are
 * loaded with RPL=3 so the IRETQ actually drops to ring 3. */
#define SEL_USER_CODE 0x1B /* 0x18 | RPL 3 */
#define SEL_USER_DATA 0x23 /* 0x20 | RPL 3 */

#define STACK_SIZE 16384   /* 16 KiB kernel and user stacks */
#define FRAME_WORDS 14     /* 9 caller-saved + 5 IRETQ fields */

/* Virtual address where user stacks live. Each task gets its own
 * 1 MiB slot so stacks never collide; pages are mapped with VMM_USER
 * (the kernel heap is supervisor-only and ring 3 must not touch it). */
#define USER_STACK_BASE 0x60000000ull
#define USER_STACK_SLOT (1ull << 20)

static struct task g_tasks[TASK_MAX];
static struct task *g_current;
static uint32_t g_next_pid = 1;
static int g_preempt_depth; /* >0: kernel code must not be switched */

static struct task *task_find_slot(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == 0) { /* unused slot */
            return &g_tasks[i];
        }
    }
    return NULL;
}

/* Round-robin: next live task after the current one. */
static struct task *task_next(struct task *after) {
    for (int i = 1; i <= TASK_MAX; i++) {
        struct task *t = &g_tasks[(after - g_tasks + i) % TASK_MAX];
        if (t->state == TASK_READY || t->state == TASK_RUNNING) {
            return t;
        }
    }
    return after;
}

struct task *sched_current(void) {
    return g_current;
}

int sched_task_count(void) {
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_READY || g_tasks[i].state == TASK_RUNNING) {
            n++;
        }
    }
    return n;
}

void task_list_all(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = &g_tasks[i];
        if (t->state == 0) {
            continue;
        }
        const char *state = t->state == TASK_RUNNING ? "running"
                          : t->state == TASK_READY  ? "ready"
                          : "zombie";
        kprintf("%-4u %-8s %s\n", t->pid, state, t->name);
    }
}

void sched_init(void) {
    g_current = task_find_slot();
    g_current->pid = g_next_pid++;
    g_current->state = TASK_RUNNING;
    strcpy(g_current->name, "tsh");
    /* Task 0 runs on the boot stack; give it a real kernel stack so
     * TSS.RSP0 is always valid. */
    g_current->kstack = (uint64_t)(uintptr_t)kmalloc(STACK_SIZE);
    g_current->kstack_top = g_current->kstack + STACK_SIZE;
    g_current->rsp = 0;
    tss_set_rsp0(g_current->kstack_top);
}

int task_create_user(uint64_t entry, const char *name) {
    struct task *t = task_find_slot();
    if (t == NULL) {
        return -1;
    }

    uint8_t *kstack = kmalloc(STACK_SIZE);
    if (kstack == NULL) {
        return -1;
    }

    /* Map a fresh user stack: ring 3 needs USER pages; the kernel
     * heap is supervisor-only. Each task gets a private slot. */
    uint64_t ustack = USER_STACK_BASE +
                      (uint64_t)(t - g_tasks) * USER_STACK_SLOT;
    uint64_t pages = STACK_SIZE / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return -1;
        }
        if (vmm_map_page(ustack + i * 4096, frame,
                         VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
            return -1;
        }
    }

    t->pid = g_next_pid++;
    t->state = TASK_READY;
    strncpy(t->name, name, TASK_NAME_MAX - 1);
    t->name[TASK_NAME_MAX - 1] = '\0';

    t->kstack = (uint64_t)(uintptr_t)kstack;
    t->kstack_top = t->kstack + STACK_SIZE;
    t->ustack = ustack;
    t->ustack_top = ustack + STACK_SIZE;

    /* Build the fake interrupt frame at the top of the kernel stack.
     * Layout (lowest first, matching what the CPU pushes on a
     * ring-3->ring-0 interrupt, plus the 9 caller-saved registers the
     * stub pushes):
     *   [r11 r10 r9 r8 rdi rsi rdx rcx rax] [rip cs rflags rsp ss]
     * The stub pops the 9 registers, IRETQ consumes the rest and the
     * task starts at `entry` in ring 3. */
    uint64_t *f = (uint64_t *)(uintptr_t)t->kstack_top;
    f[-1]  = SEL_USER_DATA;   /* ss  */
    f[-2]  = t->ustack_top;   /* rsp */
    /* rflags: IF set + IOPL=3. IRETQ to a lower privilege level
     * requires IOPL >= new CPL (else #GP); user code here runs with
     * I/O privileges for now, tightened when the syscall gate is the
     * only hardware entry point. */
    f[-3]  = 0x3202;          /* rflags: IF, IOPL=3, reserved bit 1 */
    f[-4]  = SEL_USER_CODE;   /* cs  */
    f[-5]  = entry;           /* rip */
    for (int i = 6; i <= FRAME_WORDS; i++) {
        f[-i] = 0;            /* caller-saved registers: don't care */
    }
    t->rsp = t->kstack_top - FRAME_WORDS * 8;

    return (int)t->pid;
}

/*
 * IRQ0 entry stub. Saves the caller-saved registers, calls
 * sched_tick(), and either resumes the current task or switches RSP
 * to the next task's frame and returns through it.
 */
__attribute__((naked)) void sched_tick_entry(void) {
    __asm__ volatile(
        "push %rax\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "mov %rsp, %rdi\n\t"
        "call sched_tick\n\t"
        "test %rax, %rax\n\t"
        "jz 1f\n\t"
        "mov %rax, %rsp\n\t"
        "1:\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rax\n\t"
        "iretq\n\t");
}

void preempt_disable(void) {
    g_preempt_depth++;
}

void preempt_enable(void) {
    if (g_preempt_depth > 0) {
        g_preempt_depth--;
    }
}

uint64_t sched_tick(uint64_t frame_rsp) {
    /* Acknowledge the PIT interrupt. */
    pic_send_eoi(0);
    pit_tick();

    if (g_current == NULL || g_current->state == TASK_ZOMBIE) {
        return 0; /* nothing sensible to do; stay put */
    }

    /* Preempt unless we are inside a critical section (kprintf,
     * syscall dispatch). The interrupted task is resumed later via
     * its saved frame, so switching at any non-critical point is
     * safe - including the shell's hlt() idle wait, which is exactly
     * when user tasks must get CPU time. */
    if (g_preempt_depth > 0) {
        return 0; /* critical section: no preemption */
    }

    g_current->rsp = frame_rsp;
    struct task *next = task_next(g_current);
    if (next == g_current) {
        return 0; /* only one runnable task: no switch */
    }

    g_current->state = TASK_READY;
    next->state = TASK_RUNNING;
    tss_set_rsp0(next->kstack_top);
    g_current = next;
    return next->rsp;
}

void task_exit(int status) __attribute__((noreturn));
void task_exit(int status) {
    if (g_current == NULL) {
        for (;;) {
            cli();
            hlt();
        }
    }
    g_current->state = TASK_ZOMBIE;
    g_current->exit_status = status;

    struct task *next = task_next(g_current);
    if (next == g_current) {
        /* No other task: halt the system. */
        console_write("task_exit: no other tasks - system halted.\n");
        for (;;) {
            cli();
            hlt();
        }
    }

    next->state = TASK_RUNNING;
    tss_set_rsp0(next->kstack_top);
    g_current = next;

    /* Switch to the next task's frame without returning. */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rax\n\t"
        "iretq\n\t" : : "r"(next->rsp) : "memory");
    __builtin_unreachable();
}
