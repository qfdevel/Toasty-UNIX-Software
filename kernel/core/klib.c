/*
 * klib.c - implementation of the minimal kernel support library
 *
 * The memory routines use the x86 string instructions (rep movs/stos),
 * which are significantly faster under emulation than byte-by-byte
 * loops and are also the right choice on real hardware.
 */

#include "klib.h"
#include "console.h"

/* ---- memory ---- */

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) {
        return dest;
    }

    if (d < s) {
        /* Forward copy: safe for overlapping regions where dest < src. */
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        /* Backward copy: work from the end of both buffers. */
        d += n;
        s += n;
        __asm__ volatile("std");
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
        __asm__ volatile("cld");
    }
    return dest;
}

void *memset(void *dest, int value, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"((uint8_t)value) : "memory");
    return dest;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n > 0 && *a != '\0' && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (; *s != '\0'; s++) {
        if (*s == ch) {
            return (char *)s;
        }
    }
    return (ch == '\0') ? (char *)s : NULL;
}

unsigned long strtoul(const char *s, char **end, int base) {
    const char *p = s;
    unsigned long value = 0;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    for (;; p++) {
        unsigned digit;
        if (*p >= '0' && *p <= '9') {
            digit = (unsigned)(*p - '0');
        } else if (*p >= 'a' && *p <= 'f') {
            digit = (unsigned)(*p - 'a' + 10);
        } else if (*p >= 'A' && *p <= 'F') {
            digit = (unsigned)(*p - 'A' + 10);
        } else {
            break;
        }
        if (digit >= (unsigned)base) {
            break;
        }
        value = value * (unsigned long)base + digit;
    }

    if (end != NULL) {
        *end = (char *)p;
    }
    return value;
}

/* ---- formatted output ---- */

/* Print an unsigned value in the given base; upper selects A-F vs a-f. */
static void kput_unsigned(unsigned long long value, unsigned base, int upper,
                          void (*out)(char)) {
    char digits[24];
    int count = 0;

    if (value == 0) {
        out('0');
        return;
    }

    while (value != 0) {
        unsigned digit = (unsigned)(value % base);
        if (digit < 10) {
            digits[count++] = (char)('0' + digit);
        } else {
            digits[count++] = (char)((upper ? 'A' : 'a') + digit - 10);
        }
        value /= base;
    }

    while (count > 0) {
        out(digits[--count]);
    }
}

void kvprintf(void (*out)(char), const char *fmt, va_list args) {
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            out(*fmt);
            continue;
        }
        fmt++;

        /* Flags and width (only supported for %s). */
        int left_align = 0;
        int width = 0;
        if (*fmt == '-') {
            left_align = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length modifier: any number of 'l's means 64-bit argument. */
        int is_long = 0;
        while (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            int len = (int)strlen(s);
            int pad = width - len;
            if (!left_align) {
                while (pad-- > 0) {
                    out(' ');
                }
            }
            while (*s != '\0') {
                out(*s++);
            }
            if (left_align) {
                while (pad-- > 0) {
                    out(' ');
                }
            }
            break;
        }
        case 'c':
            out((char)va_arg(args, int));
            break;
        case 'd': {
            long long v = is_long ? va_arg(args, long long)
                                  : (long long)va_arg(args, int);
            if (v < 0) {
                out('-');
                kput_unsigned((unsigned long long)(-v), 10, 0, out);
            } else {
                kput_unsigned((unsigned long long)v, 10, 0, out);
            }
            break;
        }
        case 'u':
            kput_unsigned(is_long ? va_arg(args, unsigned long long)
                                  : (unsigned long long)va_arg(args, unsigned),
                          10, 0, out);
            break;
        case 'x':
            kput_unsigned(is_long ? va_arg(args, unsigned long long)
                                  : (unsigned long long)va_arg(args, unsigned),
                          16, 0, out);
            break;
        case 'X':
            kput_unsigned(is_long ? va_arg(args, unsigned long long)
                                  : (unsigned long long)va_arg(args, unsigned),
                          16, 1, out);
            break;
        case 'p':
            out('0');
            out('x');
            kput_unsigned((unsigned long long)(uintptr_t)va_arg(args, void *),
                          16, 0, out);
            break;
        case '%':
            out('%');
            break;
        default:
            out('%');
            out(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(console_putchar, fmt, args);
    va_end(args);
}
