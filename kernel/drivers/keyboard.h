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
    KBD_EVENT_CHAR = 0,     /* ev.c holds the ASCII character */
    KBD_EVENT_SPECIAL,      /* ev.code holds a KBD_KEY_* code */
};

/* Special (non-ASCII) keys, reported as KBD_EVENT_SPECIAL. The tty
 * layer translates them into the escape sequences a real terminal
 * sends (ESC [ A ...); the shell maps PageUp/PageDown to scrollback
 * navigation. */
enum {
    KBD_KEY_UP = 1,
    KBD_KEY_DOWN,
    KBD_KEY_LEFT,
    KBD_KEY_RIGHT,
    KBD_KEY_HOME,
    KBD_KEY_END,
    KBD_KEY_DELETE,
    KBD_KEY_INSERT,
    KBD_KEY_PAGE_UP,
    KBD_KEY_PAGE_DOWN,
};

struct kbd_event {
    int type;
    int code; /* valid when type == KBD_EVENT_SPECIAL */
    char c;   /* valid when type == KBD_EVENT_CHAR */
};

/* Register the IRQ1 handler and unmask the keyboard interrupt. */
void kbd_init(void);

/* Return the next event, blocking (halting) until one is available. */
struct kbd_event kbd_get_event(void);

/* Console input ownership. Only one task may consume keyboard events
 * at a time: the shell owns the console by default, a foreground
 * user program (kilo) takes it over on its first tty read and
 * releases it on exit (task_exit). The shell releases it before
 * spawning a program. The owner is re-checked on every wake, so a
 * task that lost ownership while blocked never eats a key. */
void kbd_input_release(long pid);
long kbd_input_owner(void);

/* Like kbd_get_event(), but only returns events while `pid` owns the
 * console input (claiming it first if it is free). While another
 * task owns it, this yields the CPU without consuming anything. */
struct kbd_event kbd_get_event_owned(long pid);

/* The shell's variant: consumes events while the console is free or
 * owned by `pid`, but never claims it - a foreground user task must
 * be able to take the console over on its first tty read. */
struct kbd_event kbd_get_event_shell(long pid);

/* Return the next character, ignoring non-character events. */
char kbd_getchar(void);

/* Return the next character, or -1 if the buffer is empty. */
int kbd_poll(void);

/* True if at least one event is buffered. */
bool kbd_has_char(void);

#endif /* TUS_DRIVERS_KEYBOARD_H */
