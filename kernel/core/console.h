/*
 * console.h - unified text console
 *
 * Every kernel message goes through this layer. Output is always
 * written to the serial port (the reliable debug channel) and, when a
 * Limine framebuffer is available, mirrored to the framebuffer text
 * console. If no framebuffer exists the system still works, serial-only.
 */

#ifndef TUS_CORE_CONSOLE_H
#define TUS_CORE_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include <limine.h>

/* Initialize serial and (if given) the framebuffer console. */
void console_init(struct limine_framebuffer *fb);

/* Write one character to every active console sink. */
void console_putchar(char c);

/* Write a NUL-terminated string to every active console sink. */
void console_write(const char *s);

/* Clear the framebuffer console (no-op on serial). */
void console_clear(void);

/* Scroll the framebuffer console one page (PageUp/PageDown). */
void console_scroll_page(int dir);

/* Set the foreground/background colors used by the framebuffer console. */
void console_set_color(uint32_t fg, uint32_t bg);

/* True if a framebuffer console is active. */
bool console_has_framebuffer(void);

#endif /* TUS_CORE_CONSOLE_H */
