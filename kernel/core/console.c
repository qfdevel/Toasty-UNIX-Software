/*
 * console.c - console layer implementation
 *
 * A simple fan-out: each character is handed to the serial driver and,
 * if a framebuffer is present, to the framebuffer text console.
 */

#include "console.h"
#include "klib.h"

#include "drivers/fb.h"
#include "drivers/serial.h"

static bool g_fb_active;

void console_init(struct limine_framebuffer *fb) {
    serial_init();
    g_fb_active = (fb != NULL) && (fb_init(fb) == 0);
}

void console_putchar(char c) {
    serial_putchar(c);
    if (g_fb_active) {
        fb_putchar(c);
    }
}

void console_write(const char *s) {
    while (*s != '\0') {
        console_putchar(*s++);
    }
}

void console_clear(void) {
    if (g_fb_active) {
        fb_clear();
    }
}

void console_scroll_page(int dir) {
    if (g_fb_active) {
        fb_scroll_page(dir);
    }
}

void console_set_color(uint32_t fg, uint32_t bg) {
    if (g_fb_active) {
        fb_set_color(fg, bg);
    }
}

void console_set_text_top(uint32_t pixel_y) {
    if (g_fb_active) {
        fb_set_text_top(pixel_y);
    }
}

bool console_has_framebuffer(void) {
    return g_fb_active;
}
