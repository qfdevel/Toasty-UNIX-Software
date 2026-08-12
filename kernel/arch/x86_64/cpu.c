/*
 * cpu.c - CPUID wrapper routines
 *
 * CPUID leaf 0 returns the vendor string spread across three registers
 * (ebx:edx:ecx, four characters each). Leaves 0x80000002..0x80000004
 * return the 48-byte model string, if the CPU supports them.
 */

#include "cpu.h"

#include <stdint.h>

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

void cpu_get_vendor(char out[13]) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    __builtin_memcpy(out + 0, &b, 4);
    __builtin_memcpy(out + 4, &d, 4);
    __builtin_memcpy(out + 8, &c, 4);
    out[12] = '\0';
}

void cpu_get_brand(char out[49]) {
    uint32_t a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x80000004) {
        out[0] = '\0'; /* CPU does not provide a model string */
        return;
    }

    char *cursor = out;
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        cpuid(leaf, &a, &b, &c, &d);
        __builtin_memcpy(cursor + 0, &a, 4);
        __builtin_memcpy(cursor + 4, &b, 4);
        __builtin_memcpy(cursor + 8, &c, 4);
        __builtin_memcpy(cursor + 12, &d, 4);
        cursor += 16;
    }
    out[48] = '\0';
}
