/*
 * gdt.h - Global Descriptor Table and Task State Segment
 *
 * Selectors (see gdt.c for the full layout):
 *   0x08 kernel code, 0x10 kernel data, 0x18 user code,
 *   0x20 user data, 0x28 TSS.
 */

#ifndef TUS_ARCH_GDT_H
#define TUS_ARCH_GDT_H

#include <stdint.h>

/* Install the TUS GDT (kernel + user segments + TSS) and load the
 * task register. Call before enabling interrupts. */
void gdt_init(void);

/* Point TSS.RSP0 at a kernel stack; call on every task switch so
 * interrupts from user mode land on the current task's stack. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* TUS_ARCH_GDT_H */
