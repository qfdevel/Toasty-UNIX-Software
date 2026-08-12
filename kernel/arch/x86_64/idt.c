/*
 * idt.c - Interrupt Descriptor Table implementation
 *
 * All 256 IDT slots are populated:
 *   - 0..31   : CPU exceptions -> fatal handler that dumps registers
 *   - 32..47  : PIC hardware interrupts -> dispatch to registered IRQ
 *               handlers, then acknowledge the PIC
 *   - 48..255 : ignored (silently returned from)
 *
 * GCC's "interrupt" attribute turns a normal C function into a full
 * interrupt service routine: it saves all clobbered registers, uses
 * IRETQ to return, and consumes a CPU-pushed error code when the
 * function declares a second parameter. This keeps the whole IDT layer
 * in portable C.
 */

#include "idt.h"
#include "pic.h"
#include "io.h"
#include "core/console.h"
#include "core/klib.h"
#include "syscall/syscall.h"

/* 64-bit interrupt gate: present, DPL 0. IF is cleared on entry. */
#define IDT_GATE_64_INTERRUPT 0x8E

/* 64-bit trap gate at DPL 3: used for the syscall vector (int 0x80),
 * callable from user mode once processes exist, without clearing IF. */
#define IDT_GATE_64_TRAP_USER 0xEF

/* Vector used for POSIX system calls. */
#define IDT_VECTOR_SYSCALL 0x80

/* 64-bit trap gate at DPL 3: used for the syscall vector (int 0x80),
 * callable from user mode once processes exist, without clearing IF. */
#define IDT_GATE_64_TRAP_USER 0xEF

/* Vector used for POSIX system calls. */
#define IDT_VECTOR_SYSCALL 0x80

/* Code segment selector the kernel runs in. The Limine boot protocol
 * guarantees that the kernel is entered with CS = 0x28 (64-bit code)
 * and DS/ES/SS/FS/GS = 0x30 (64-bit data), so interrupt gates must
 * point back at 0x28 - NOT at 0x08, which is Limine's 32-bit compat
 * segment. An IDT entry with the wrong selector makes the CPU fetch
 * the handler through a 32-bit descriptor and triple-faults. */
#define KERNEL_CODE_SELECTOR 0x28

struct idt_entry {
    uint16_t offset_low;   /* bits  0..15 of handler address */
    uint16_t selector;     /* code segment selector */
    uint8_t  ist;          /* interrupt stack table index (0 = none) */
    uint8_t  attributes;   /* gate type, DPL, present bit */
    uint16_t offset_mid;   /* bits 16..31 of handler address */
    uint32_t offset_high;  /* bits 32..63 of handler address */
    uint32_t zero;         /* reserved, must be zero */
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;        /* size of the IDT in bytes minus one */
    uint64_t base;         /* virtual address of the IDT */
} __attribute__((packed));

static struct idt_entry g_idt[256];
static struct idt_ptr g_idt_ptr;
static irq_handler_t g_irq_handlers[16];

/* Human-readable names for the first 32 vectors. */
static const char *const g_exception_names[32] = {
    "Divide Error",              "Debug",                  "Non-Maskable Interrupt",
    "Breakpoint",                "Overflow",               "Bound Range Exceeded",
    "Invalid Opcode",            "Device Not Available",   "Double Fault",
    "Coprocessor Segment Overrun","Invalid TSS",           "Segment Not Present",
    "Stack-Segment Fault",       "General Protection",     "Page Fault",
    "Reserved",                  "x87 FPU Error",          "Alignment Check",
    "Machine Check",             "SIMD FPU Exception",     "Virtualization Exception",
    "Control Protection",        "Reserved",               "Reserved",
    "Reserved",                  "Reserved",               "Reserved",
    "Reserved",                  "Hypervisor Injection",   "VMM Communication",
    "Security Exception",        "Reserved"
};

/* Vectors on which the CPU pushes an error code onto the stack.
 * The DEFINE_EXC_WITH_ERROR lines below are exactly these vectors:
 * 8, 10, 11, 12, 13, 14, 17, 21, 29, 30. */

/*
 * Fatal exception: print a register dump and stop the system. This is
 * the kernel panic path. We disable interrupts and halt forever so the
 * state stays visible on screen and on the serial log.
 */
static void exception_fatal(const struct interrupt_frame *frame,
                            uint64_t error_code, int vec, bool has_error_code) {
    console_write("\n\n*** KERNEL PANIC ***\n");
    kprintf("Exception %d: %s\n", vec, g_exception_names[vec]);
    if (has_error_code) {
        kprintf("Error code: 0x%llx\n", (unsigned long long)error_code);
    }
    if (vec == 14) { /* Page Fault: CR2 holds the faulting address. */
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("CR2     : 0x%llx\n", (unsigned long long)cr2);
    }
    kprintf("RIP    : 0x%llx\n", (unsigned long long)frame->rip);
    kprintf("CS     : 0x%llx\n", (unsigned long long)frame->cs);
    kprintf("RFLAGS : 0x%llx\n", (unsigned long long)frame->rflags);
    kprintf("RSP    : 0x%llx\n", (unsigned long long)frame->rsp);
    kprintf("SS     : 0x%llx\n", (unsigned long long)frame->ss);
    console_write("System halted.\n");
    for (;;) {
        cli();
        hlt();
    }
}

/* One generated handler per exception vector; the two families differ
 * only in whether the CPU pushes an error code for that vector. */
#define DEFINE_EXC_NO_ERROR(vec)                                          \
    __attribute__((interrupt)) static void exc_no_error_##vec(            \
        struct interrupt_frame *frame) {                                  \
        exception_fatal(frame, 0, vec, false);                            \
    }

#define DEFINE_EXC_WITH_ERROR(vec)                                        \
    __attribute__((interrupt)) static void exc_with_error_##vec(          \
        struct interrupt_frame *frame, uint64_t error_code) {             \
        exception_fatal(frame, error_code, vec, true);                    \
    }

DEFINE_EXC_NO_ERROR(0)  DEFINE_EXC_NO_ERROR(1)  DEFINE_EXC_NO_ERROR(2)
DEFINE_EXC_NO_ERROR(3)  DEFINE_EXC_NO_ERROR(4)  DEFINE_EXC_NO_ERROR(5)
DEFINE_EXC_NO_ERROR(6)  DEFINE_EXC_NO_ERROR(7)  DEFINE_EXC_WITH_ERROR(8)
DEFINE_EXC_NO_ERROR(9)  DEFINE_EXC_WITH_ERROR(10) DEFINE_EXC_WITH_ERROR(11)
DEFINE_EXC_WITH_ERROR(12) DEFINE_EXC_WITH_ERROR(13) DEFINE_EXC_WITH_ERROR(14)
DEFINE_EXC_NO_ERROR(15) DEFINE_EXC_NO_ERROR(16) DEFINE_EXC_WITH_ERROR(17)
DEFINE_EXC_NO_ERROR(18) DEFINE_EXC_NO_ERROR(19) DEFINE_EXC_NO_ERROR(20)
DEFINE_EXC_WITH_ERROR(21) DEFINE_EXC_NO_ERROR(22) DEFINE_EXC_NO_ERROR(23)
DEFINE_EXC_NO_ERROR(24) DEFINE_EXC_NO_ERROR(25) DEFINE_EXC_NO_ERROR(26)
DEFINE_EXC_NO_ERROR(27) DEFINE_EXC_NO_ERROR(28) DEFINE_EXC_WITH_ERROR(29)
DEFINE_EXC_WITH_ERROR(30) DEFINE_EXC_NO_ERROR(31)

/* Vector -> handler lookup tables. Function pointers are stored as
 * integers to keep the tables free of pedantic cast warnings. */
#define STUB_CAST(fn) ((uintptr_t)(fn))

static const uintptr_t g_exception_stubs[32] = {
    [0]  = STUB_CAST(exc_no_error_0),   [1]  = STUB_CAST(exc_no_error_1),
    [2]  = STUB_CAST(exc_no_error_2),   [3]  = STUB_CAST(exc_no_error_3),
    [4]  = STUB_CAST(exc_no_error_4),   [5]  = STUB_CAST(exc_no_error_5),
    [6]  = STUB_CAST(exc_no_error_6),   [7]  = STUB_CAST(exc_no_error_7),
    [8]  = STUB_CAST(exc_with_error_8), [9]  = STUB_CAST(exc_no_error_9),
    [10] = STUB_CAST(exc_with_error_10),[11] = STUB_CAST(exc_with_error_11),
    [12] = STUB_CAST(exc_with_error_12),[13] = STUB_CAST(exc_with_error_13),
    [14] = STUB_CAST(exc_with_error_14),[15] = STUB_CAST(exc_no_error_15),
    [16] = STUB_CAST(exc_no_error_16),[17] = STUB_CAST(exc_with_error_17),
    [18] = STUB_CAST(exc_no_error_18),[19] = STUB_CAST(exc_no_error_19),
    [20] = STUB_CAST(exc_no_error_20),[21] = STUB_CAST(exc_with_error_21),
    [22] = STUB_CAST(exc_no_error_22),[23] = STUB_CAST(exc_no_error_23),
    [24] = STUB_CAST(exc_no_error_24),[25] = STUB_CAST(exc_no_error_25),
    [26] = STUB_CAST(exc_no_error_26),[27] = STUB_CAST(exc_no_error_27),
    [28] = STUB_CAST(exc_no_error_28),[29] = STUB_CAST(exc_with_error_29),
    [30] = STUB_CAST(exc_with_error_30),[31] = STUB_CAST(exc_no_error_31),
};

/*
 * Common path for all hardware interrupts: run the registered handler
 * (if any) and acknowledge the PIC so the next interrupt can arrive.
 */
static void irq_dispatch(uint8_t irq, struct interrupt_frame *frame) {
    /*
     * Spurious IRQ7/IRQ15: the PIC can raise these when no device is
     * actually asserting an interrupt. Only acknowledge them if the
     * in-service bit really is set, otherwise the cascade would get
     * stuck.
     */
    if (irq == 7 || irq == 15) {
        uint16_t cmd_port = (irq == 7) ? PIC1_CMD_PORT : PIC2_CMD_PORT;
        outb(cmd_port, PIC_READ_ISR);
        if (!(inb(cmd_port) & (1u << (irq & 7)))) {
            return; /* spurious - no EOI */
        }
    }

    if (g_irq_handlers[irq] != NULL) {
        g_irq_handlers[irq](frame);
    }
    pic_send_eoi(irq);
}

/* One IRETQ stub per PIC IRQ line. */
#define DEFINE_IRQ_STUB(irq)                                              \
    __attribute__((interrupt)) static void irq_stub_##irq(                \
        struct interrupt_frame *frame) {                                  \
        irq_dispatch(irq, frame);                                         \
    }

DEFINE_IRQ_STUB(0)  DEFINE_IRQ_STUB(1)  DEFINE_IRQ_STUB(2)  DEFINE_IRQ_STUB(3)
DEFINE_IRQ_STUB(4)  DEFINE_IRQ_STUB(5)  DEFINE_IRQ_STUB(6)  DEFINE_IRQ_STUB(7)
DEFINE_IRQ_STUB(8)  DEFINE_IRQ_STUB(9)  DEFINE_IRQ_STUB(10) DEFINE_IRQ_STUB(11)
DEFINE_IRQ_STUB(12) DEFINE_IRQ_STUB(13) DEFINE_IRQ_STUB(14) DEFINE_IRQ_STUB(15)

static const uintptr_t g_irq_stubs[16] = {
    [0] = STUB_CAST(irq_stub_0),  [1] = STUB_CAST(irq_stub_1),
    [2] = STUB_CAST(irq_stub_2),  [3] = STUB_CAST(irq_stub_3),
    [4] = STUB_CAST(irq_stub_4),  [5] = STUB_CAST(irq_stub_5),
    [6] = STUB_CAST(irq_stub_6),  [7] = STUB_CAST(irq_stub_7),
    [8] = STUB_CAST(irq_stub_8),  [9] = STUB_CAST(irq_stub_9),
    [10] = STUB_CAST(irq_stub_10),[11] = STUB_CAST(irq_stub_11),
    [12] = STUB_CAST(irq_stub_12),[13] = STUB_CAST(irq_stub_13),
    [14] = STUB_CAST(irq_stub_14),[15] = STUB_CAST(irq_stub_15),
};

/* Vectors 48..255: no device behind them; just return. */
__attribute__((interrupt)) static void irq_ignored(struct interrupt_frame *frame) {
    (void)frame;
}

static void idt_set_gate_attr(int vector, uintptr_t handler, uint8_t attributes) {
    g_idt[vector].offset_low  = (uint16_t)(handler & 0xFFFF);
    g_idt[vector].selector    = KERNEL_CODE_SELECTOR;
    g_idt[vector].ist         = 0;
    g_idt[vector].attributes  = attributes;
    g_idt[vector].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    g_idt[vector].zero        = 0;
}

static void idt_set_gate(int vector, uintptr_t handler) {
    idt_set_gate_attr(vector, handler, IDT_GATE_64_INTERRUPT);
}

void idt_init(void) {
    for (int vector = 0; vector < 256; vector++) {
        if (vector < 32) {
            idt_set_gate(vector, g_exception_stubs[vector]);
        } else if (vector < 48) {
            idt_set_gate(vector, g_irq_stubs[vector - 32]);
        } else {
            idt_set_gate(vector, STUB_CAST(irq_ignored));
        }
    }

    /* POSIX system call gate (int 0x80), trap gate at DPL 3. */
    idt_set_gate_attr(IDT_VECTOR_SYSCALL, STUB_CAST(syscall_entry),
                      IDT_GATE_64_TRAP_USER);

    g_idt_ptr.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_ptr.base  = (uint64_t)(uintptr_t)g_idt;
    __asm__ volatile("lidt %0" : : "m"(g_idt_ptr));
}

void irq_install(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        g_irq_handlers[irq] = handler;
    }
}
