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
#include "drivers/keyboard.h"
#include "vfs/vfs.h"

/* Selectors from gdt.c (duplicated here to keep sched.c self-contained
 * for the fake IRETQ frame; they must match gdt.c). User segments are
 * loaded with RPL=3 so the IRETQ actually drops to ring 3. */
#define SEL_USER_CODE 0x1B /* 0x18 | RPL 3 */
#define SEL_USER_DATA 0x23 /* 0x20 | RPL 3 */

#define STACK_SIZE 16384   /* kernel stacks: 16 KiB */
#define USER_STACK_SIZE 65536 /* user stacks: 64 KiB (grep/sed recursion) */
/* Frame words: 9 caller-saved + 6 callee-saved (rbx rbp r12-r15) + 5
 * IRETQ fields. The callee-saved registers MUST travel with the task:
 * a tick can preempt ring-0 code mid-function, detour through a
 * ring-3 task that clobbers them, and resume the interrupted code -
 * which then computes with garbage register values (the fb_scroll_up
 * corruption bug). */
#define FRAME_WORDS 20     /* 15 registers + 5 IRETQ fields */

/* Virtual address where user stacks live. Each task has its own
 * address space, so every task can use the same stack address; the
 * pages are mapped with VMM_USER in the task's private space (the
 * kernel heap is supervisor-only and ring 3 must not touch it). */
#define USER_STACK_BASE 0x60000000ull

/* First address handed out by SYS_MMAP (must match syscall.c). */
#define MMAP_CURSOR_START 0x40000000ull

/* IA32_FS_BASE model-specific register (see io.h wrmsr/rdmsr). */
#define MSR_FS_BASE 0xC0000100

/* AT_PAGESZ auxv entry: the C library (musl) reads the page size
 * from the auxiliary vector at startup; without it the allocator
 * breaks (page_size 0). */
#define AT_PAGESZ 6

static struct task g_tasks[TASK_MAX];
static struct task *g_current;
static uint32_t g_next_pid = 1;
static int g_preempt_depth; /* >0: kernel code must not be switched */

static struct task *task_find_slot(void) {
    /* Reuse a zombie slot first (the task finished; its kernel stack
     * and address space are reclaimed lazily), then an unused one.
     * Without recycling, TASK_MAX (16) spawned programs would fill
     * the table and later execs would fail. */
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_ZOMBIE) {
            memset(&g_tasks[i], 0, sizeof(g_tasks[i]));
            return &g_tasks[i];
        }
    }
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

int sched_task_alive(uint32_t pid) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].pid == pid && g_tasks[i].state != TASK_ZOMBIE) {
            return 1;
        }
    }
    return 0;
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
        kprintf("%-4u %-8s %-10llx %s\n", t->pid, state,
                (unsigned long long)t->cr3, t->name);
    }
}

void sched_init(void) {
    g_current = task_find_slot();
    g_current->pid = g_next_pid++;
    g_current->state = TASK_RUNNING;
    strcpy(g_current->name, "tsh");
    /* Task 0 is the kernel shell: it keeps the boot address space. */
    g_current->cr3 = vmm_root_cr3();
    g_current->fs_base = 0;
    g_current->mmap_cur = MMAP_CURSOR_START;
    /* Task 0 runs on the boot stack; give it a real kernel stack so
     * TSS.RSP0 is always valid. */
    g_current->kstack = (uint64_t)(uintptr_t)kmalloc(STACK_SIZE);
    g_current->kstack_top = g_current->kstack + STACK_SIZE;
    g_current->rsp = 0;
    /* Capture the boot CPU's FPU state so switching back to task 0
     * restores exactly what the shell left behind. */
    __asm__ volatile("fxsave (%0)" : : "r"(g_current->fpu) : "memory");
    wrmsr(MSR_FS_BASE, 0);
    tss_set_rsp0(g_current->kstack_top);
}

int task_create_user(uint64_t entry, const char *name, uint64_t cr3,
                     int argc, char **argv) {
    struct task *t = task_find_slot();
    if (t == NULL) {
        return -1;
    }

    uint8_t *kstack = kmalloc(STACK_SIZE);
    if (kstack == NULL) {
        return -1;
    }

    /* Map a fresh user stack inside the task's private address space:
     * ring 3 needs USER pages there. The kernel heap (where this
     * code runs) is supervisor-only. Frames are zeroed via the HHDM
     * mapping: the C library's startup code expects a clean stack. */
    uint64_t ustack = USER_STACK_BASE;
    uint64_t pages = USER_STACK_SIZE / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (frame == 0) {
            return -1;
        }
        memset((void *)pmm_phys_to_virt(frame), 0, 4096);
        if (vmm_map_page_in(cr3, ustack + i * 4096, frame,
                            VMM_PRESENT | VMM_WRITE | VMM_USER) != 0) {
            return -1;
        }
    }

    /* Initial user stack image, laid out the way a C runtime expects
     * it (crt1 reads argc at (%rsp), argv after it, then a NULL
     * envp, then the auxiliary vector):
     *   [0] argc
     *   [1..argc] argv[0..argc-1] pointers
     *   [argc+1] NULL (argv terminator)
     *   [argc+2] envp[0] = NULL
     *   [argc+3] auxv[0] = AT_PAGESZ
     *   [argc+4] auxv[1] = 4096
     *   [argc+5] auxv[2] = 0 (terminator)
     * RSP points at word [0]. The argument strings are copied just
     * below the pointer array (still inside the top stack page) and
     * the pointers are written as user virtual addresses, since the
     * kernel heap is not readable from ring 3. All writes go through
     * the HHDM mapping (the caller may not be in this space). */
    int nargs = argc + 1;                 /* program name + arguments */
    int nwords = nargs + 6;               /* argc + argv[] + NULL +
                                             envp[0] + 2 auxv + term */
    if (nargs > 16) {
        nargs = 16;
        nwords = nargs + 6;
    }
    uint64_t top_frame = vmm_translate_in(cr3, ustack + USER_STACK_SIZE - 4096);
    if (top_frame == 0) {
        return -1;
    }
    char *frame_base = (char *)pmm_phys_to_virt(top_frame);

    /* Argument strings live at the VERY TOP of the top page, growing
     * downward; the pointer array sits below them and the initial RSP
     * below that. The stack grows down from RSP, so it can never
     * reach the strings - with the strings at the BOTTOM of the page
     * (older layout) a program using more than ~4 KiB of stack
     * silently corrupted argv. All writes go through the HHDM
     * mapping (the caller may not be in this space); the pointers
     * stored in the array are user virtual addresses. */
    const char *args[16];
    args[0] = name != NULL ? name : "";
    for (int i = 1; i < nargs; i++) {
        args[i] = (argv != NULL && argv[i - 1] != NULL) ? argv[i - 1] : "";
    }
    size_t need = 8 + (size_t)nwords * 8;
    for (int i = 0; i < nargs; i++) {
        need += strlen(args[i]) + 1;
    }
    if (need > 4096) {
        return -1;
    }

    char *sptr = frame_base + 4096;
    uint64_t arg_ptrs[16];
    for (int i = 0; i < nargs; i++) {
        size_t slen = strlen(args[i]);
        sptr -= slen + 1;
        memcpy(sptr, args[i], slen + 1);
        arg_ptrs[i] = (uint64_t)(ustack + USER_STACK_SIZE - 4096) +
                      (uint64_t)(sptr - frame_base);
    }

    uint64_t *init = (uint64_t *)((uintptr_t)sptr & ~(uintptr_t)7);
    init -= nwords;
    init[0] = (uint64_t)nargs;   /* argc */
    for (int i = 0; i < nargs; i++) {
        init[1 + i] = arg_ptrs[i];
    }
    init[1 + nargs] = 0;        /* argv terminator */
    init[2 + nargs] = 0;        /* envp[0] */
    init[3 + nargs] = AT_PAGESZ;
    init[4 + nargs] = 4096;     /* page size */
    init[5 + nargs] = 0;        /* auxv terminator */
    uint64_t ustack_rsp = (uint64_t)(ustack + USER_STACK_SIZE - 4096) +
                          (uint64_t)((char *)init - frame_base);

    t->pid = g_next_pid++;
    t->state = TASK_READY;
    strncpy(t->name, name, TASK_NAME_MAX - 1);
    t->name[TASK_NAME_MAX - 1] = '\0';

    t->kstack = (uint64_t)(uintptr_t)kstack;
    t->kstack_top = t->kstack + STACK_SIZE;
    t->ustack = ustack;
    t->ustack_top = ustack + USER_STACK_SIZE;
    t->cr3 = cr3;
    t->fs_base = 0;
    t->mmap_cur = MMAP_CURSOR_START;
    t->uid = 0;   /* every task starts as root (no privilege model yet) */
    t->euid = 0;
    t->gid = 0;
    t->egid = 0;

    /* Inherit the creator's open files: the child runs with the same
     * fds the shell set up (redirection/pipes) and the shell can
     * restore its own slots immediately after spawning. */
    vfs_fd_inherit(t->fds, g_current->fds);

    /* Default FPU state (hardware reset values): x87 control word
     * with all exceptions masked, empty x87 tag word, MXCSR with all
     * exceptions masked. fxrstor of a fully zeroed image would leave
     * MXCSR unmasked (NaN operations would raise #XM). */
    memset(t->fpu, 0, sizeof(t->fpu));
    *(uint16_t *)(t->fpu + 0) = 0x037F;  /* x87 CW */
    *(uint16_t *)(t->fpu + 4) = 0xFFFF;  /* x87 FTW (all empty) */
    *(uint32_t *)(t->fpu + 24) = 0x1F80; /* MXCSR */
    *(uint32_t *)(t->fpu + 28) = 0xFFFF; /* MXCSR_MASK */

    /* Build the fake interrupt frame at the top of the kernel stack.
     * Layout (lowest first, matching what the CPU pushes on a
     * ring-3->ring-0 interrupt, plus the 15 registers the stub
     * pushes):
     *   [r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rdi rsi rdx rcx rax]
     *   [rip cs rflags rsp ss]
     * The stub pops the 15 registers, IRETQ consumes the rest and
     * the task starts at `entry` in ring 3. */
    uint64_t *f = (uint64_t *)(uintptr_t)t->kstack_top;
    f[-1]  = SEL_USER_DATA;   /* ss  */
    f[-2]  = ustack_rsp;      /* rsp: points at the init stack image */
    /* rflags: IF set + IOPL=3. IRETQ to a lower privilege level
     * requires IOPL >= new CPL (else #GP); user code here runs with
     * I/O privileges for now, tightened when the syscall gate is the
     * only hardware entry point. */
    f[-3]  = 0x3202;          /* rflags: IF, IOPL=3, reserved bit 1 */
    f[-4]  = SEL_USER_CODE;   /* cs  */
    f[-5]  = entry;           /* rip */
    for (int i = 6; i <= FRAME_WORDS; i++) {
        f[-i] = 0;            /* registers: don't care for a fresh task */
    }
    t->rsp = t->kstack_top - FRAME_WORDS * 8;

    return (int)t->pid;
}

/*
 * IRQ0 entry stub. Saves ALL general-purpose registers (caller- and
 * callee-saved), calls sched_tick(), and either resumes the current
 * task or switches RSP to the next task's frame and returns through
 * it. Saving the callee-saved registers is essential: the interrupted
 * kernel code may be mid-computation (e.g. fb_putchar's scroll) and
 * relies on rbx/rbp/r12-r15 surviving a detour through a ring-3 task.
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
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rsp, %rdi\n\t"
        "call sched_tick\n\t"
        "test %rax, %rax\n\t"
        "jz 1f\n\t"
        "mov %rax, %rsp\n\t"
        "1:\n\t"
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
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
    /* Save the outgoing task's FPU state before it is switched away;
     * restore the incoming task's state after the address-space
     * switch. Same for the FS base (thread pointer / TLS). */
    __asm__ volatile("fxsave (%0)" : : "r"(g_current->fpu) : "memory");
    /* Load the next task's address space. This is safe here: the
     * kernel stack and all kernel data are mapped in every space via
     * the shared kernel half. The CR3 reload also flushes the TLB. */
    vmm_space_switch(next->cr3);
    __asm__ volatile("fxrstor (%0)" : : "r"(next->fpu) : "memory");
    wrmsr(MSR_FS_BASE, next->fs_base);
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

    /* A foreground program (kilo) that owned the console keyboard
     * must give it back so the shell can read again. */
    kbd_input_release(g_current->pid);

    /* Release the task's file descriptors: the fd table is per-task,
     * and closing the whole table is also what lets a pipe reader
     * see EOF once its writer exits. */
    vfs_close_all();

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
    /* Same address-space switch as in sched_tick: the iretq below
     * resumes the next task inside its own space. FPU state and the
     * FS base travel with the task as well. */
    __asm__ volatile("fxsave (%0)" : : "r"(g_current->fpu) : "memory");
    vmm_space_switch(next->cr3);
    __asm__ volatile("fxrstor (%0)" : : "r"(next->fpu) : "memory");
    wrmsr(MSR_FS_BASE, next->fs_base);
    g_current = next;

    /* Switch to the next task's frame without returning. The pop order
 * mirrors sched_tick_entry's pushes (callee-saved first). */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%rbp\n\t"
        "pop %%rbx\n\t"
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
