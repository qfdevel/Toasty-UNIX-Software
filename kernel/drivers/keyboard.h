/*
 * keyboard.h - PS/2 keyboard driver
 *
 * Interrupt-driven driver for the standard PS/2 keyboard (scancode
 * set 1). The IRQ1 handler decodes scancodes into ASCII characters,
 * honoring Shift, Caps Lock and Ctrl, and stores them in an internal
 * ring buffer. The shell consumes characters with kbd_getchar().
 */

#ifndef TUS_DRIVERS_KEYBOARD_H
#define TUS_DRIVERS_KEYBOARD_H

#include <stdbool.h>

/* Register the IRQ1 handler and unmask the keyboard interrupt. */
void kbd_init(void);

/* Return the next character, blocking (halting) until one is available. */
char kbd_getchar(void);

/* Return the next character, or -1 if the buffer is empty. */
int kbd_poll(void);

/* True if at least one character is buffered. */
bool kbd_has_char(void);

#endif /* TUS_DRIVERS_KEYBOARD_H */
