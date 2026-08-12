/*
 * klib.h - minimal kernel support library
 *
 * A freestanding kernel cannot use the host C library, so we provide
 * the small set of helpers every subsystem needs: memory routines and
 * formatted output. kprintf() writes through the console abstraction,
 * which mirrors output to serial and (optionally) the framebuffer.
 */

#ifndef TUS_CORE_KLIB_H
#define TUS_CORE_KLIB_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---- memory ---- */

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int value, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

/* ---- formatted output ----
 *
 * Supported conversions: %s %c %d %u %x %X %p %%
 * The 'l' modifier (or 'll') promotes the argument to 64 bits.
 * %s accepts an optional width and '-' flag, e.g. "%-10s".
 */

/* Write formatted text through an arbitrary character sink. */
void kvprintf(void (*sink)(char), const char *fmt, va_list args);

/* Write formatted text to the active console. */
void kprintf(const char *fmt, ...);

#endif /* TUS_CORE_KLIB_H */
