/*
 * io.h - x86_64 port I/O and CPU control primitives
 *
 * Everything in this header is a static inline function; there is no
 * code in a separate translation unit, which keeps the kernel binary
 * small and these hot paths as fast as possible.
 */

#ifndef TUS_ARCH_IO_H
#define TUS_ARCH_IO_H

#include <stdint.h>

/* Write one byte to an I/O port. */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Read one byte from an I/O port. */
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Write one 16-bit word to an I/O port. */
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* Read one 16-bit word from an I/O port. */
static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/*
 * Briefly stall the I/O bus. The legacy PC has a well-known quirk where
 * consecutive writes to certain chips (notably the PIC) can be lost;
 * a dummy write to port 0x80 (an unused "post" port) in between gives
 * the hardware time to settle.
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}

/* Clear the interrupt flag: mask all maskable interrupts. */
static inline void cli(void) {
    __asm__ volatile("cli");
}

/* Set the interrupt flag: unmask all maskable interrupts. */
static inline void sti(void) {
    __asm__ volatile("sti");
}

/* Halt the CPU until the next interrupt arrives. */
static inline void hlt(void) {
    __asm__ volatile("hlt");
}

#endif /* TUS_ARCH_IO_H */
