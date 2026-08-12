/*
 * keyboard.h - PS/2 keyboard driver
 *
 * Interrupt-driven driver for the standard PS/2 keyboard (scancode
 * set 1). The IRQ1 handler decodes scancodes into events - ASCII
 * characters (honoring Shift, Caps Lock and Ctrl) plus special keys
 * such as PageUp/PageDown for scrollback navigation - and stores them
 * in an internal ring buffer. Consumers block on kbd_get_event().
 */

#ifndef TUS_DRIVERS_KEYBOARD_H
#define TUS_DRIVERS_KEYBOARD_H

#include <stdbool.h>

/* Event types produced by the keyboard. */
enum {
    KBD_EVENT_CHAR = 0, /* ev.c holds the ASCII character */
    KBD_EVENT_SCROLL_UP,   /* PageUp: look back into scrollback */
    KBD_EVENT_SCROLL_DOWN, /* PageDown: return towards live output */
};

struct kbd_event {
    int type;
    char c; /* valid when type == KBD_EVENT_CHAR */
};

/* Register the IRQ1 handler and unmask the keyboard interrupt. */
void kbd_init(void);

/* Return the next event, blocking (halting) until one is available. */
struct kbd_event kbd_get_event(void);

/* Return the next character, ignoring non-character events. */
char kbd_getchar(void);

/* Return the next character, or -1 if the buffer is empty. */
int kbd_poll(void);

/* True if at least one event is buffered. */
bool kbd_has_char(void);

#endif /* TUS_DRIVERS_KEYBOARD_H */
