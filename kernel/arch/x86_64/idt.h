/*
 * idt.h - Interrupt Descriptor Table (x86_64)
 *
 * The IDT maps CPU exceptions (vectors 0..31) and hardware interrupts
 * (vectors 32..47, after PIC remapping) to handler functions. Handlers
 * are written in C using GCC's "interrupt" attribute, so no assembly
 * stubs are needed.
 */

#ifndef TUS_ARCH_IDT_H
#define TUS_ARCH_IDT_H

#include <stdint.h>

/*
 * Register frame pushed by the CPU on every interrupt/exception.
 * Field order matches the hardware stack layout (lowest address first).
 * For exceptions that carry an error code, the error code is pushed
 * below this frame and passed as a separate argument to the handler.
 */
struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* Signature of a registered hardware interrupt (IRQ) handler. */
typedef void (*irq_handler_t)(struct interrupt_frame *frame);

/* Build and load the IDT; installs exception and IRQ gate stubs. */
void idt_init(void);

/* Attach a handler to one of the 16 PIC IRQ lines (0..15). */
void irq_install(uint8_t irq, irq_handler_t handler);

#endif /* TUS_ARCH_IDT_H */
