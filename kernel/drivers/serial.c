/*
 * serial.c - 16550 UART driver implementation
 *
 * Register offsets are relative to the COM1 base port 0x3F8. The baud
 * rate is set through a divisor register that is only reachable while
 * the "DLAB" bit of the line control register is set.
 */

#include "serial.h"

#include "arch/x86_64/io.h"

#define COM1_BASE 0x3F8

/* Divisor for 115200 baud (1.8432 MHz / 16 / 115200 = 1). */
#define BAUD_DIVISOR 1

#define REG_DATA 0 /* data register (also divisor LSB with DLAB) */
#define REG_IER  1 /* interrupt enable (also divisor MSB with DLAB) */
#define REG_FCR  2 /* FIFO control */
#define REG_LCR  3 /* line control */
#define REG_MCR  4 /* modem control */
#define REG_LSR  5 /* line status */

#define LCR_DLAB      0x80 /* divisor latch access bit */
#define LCR_8N1       0x03 /* 8 data bits, no parity, 1 stop bit */
#define FCR_ENABLE    0xC7 /* enable FIFOs, clear them, 14-byte threshold */
#define MCR_DTR_RTS   0x0B /* DTR + RTS + OUT2 (needed for interrupts) */

#define LSR_THR_EMPTY 0x20 /* transmit holding register empty */

static bool g_ready;

bool serial_init(void) {
    outb(COM1_BASE + REG_IER, 0x00); /* disable all UART interrupts */

    /* Set the baud rate: enable DLAB, write the divisor, disable DLAB. */
    outb(COM1_BASE + REG_LCR, LCR_DLAB);
    outb(COM1_BASE + REG_DATA, (uint8_t)(BAUD_DIVISOR & 0xFF));
    outb(COM1_BASE + REG_IER, (uint8_t)((BAUD_DIVISOR >> 8) & 0xFF));
    outb(COM1_BASE + REG_LCR, LCR_8N1);

    outb(COM1_BASE + REG_FCR, FCR_ENABLE);
    outb(COM1_BASE + REG_MCR, MCR_DTR_RTS);

    g_ready = true;
    return true;
}

void serial_putchar(char c) {
    if (!g_ready) {
        return;
    }

    /* Wait until the transmitter accepts the byte. The wait is bounded
     * so a broken port can never hang the kernel. */
    for (unsigned tries = 0; tries < 100000; tries++) {
        if (inb(COM1_BASE + REG_LSR) & LSR_THR_EMPTY) {
            break;
        }
    }
    outb(COM1_BASE + REG_DATA, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s != '\0') {
        serial_putchar(*s++);
    }
}
