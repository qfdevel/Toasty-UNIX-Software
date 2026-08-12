/*
 * serial.h - 16550 UART driver
 *
 * The serial port is the kernel's most trustworthy debug channel: it
 * works from the very first instruction and does not depend on any
 * display hardware. TUS uses COM1 at 115200 baud, 8 data bits, no
 * parity, 1 stop bit (8N1).
 */

#ifndef TUS_DRIVERS_SERIAL_H
#define TUS_DRIVERS_SERIAL_H

#include <stdbool.h>

/* Initialize COM1. Returns true on success. */
bool serial_init(void);

/* Transmit a single character (blocks until the FIFO accepts it). */
void serial_putchar(char c);

/* Transmit a NUL-terminated string. */
void serial_write(const char *s);

#endif /* TUS_DRIVERS_SERIAL_H */
