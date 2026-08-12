/*
 * cpu.h - CPU identification (CPUID)
 *
 * Exposes the vendor string and the human-readable model string that
 * every x86-64 CPU provides, used by the "sysinfo" shell command.
 */

#ifndef TUS_ARCH_CPU_H
#define TUS_ARCH_CPU_H

/* Write the 12-character vendor string (e.g. "GenuineIntel") plus NUL. */
void cpu_get_vendor(char out[13]);

/* Write the 48-character model string (e.g. "AMD Ryzen 7 ...") plus NUL. */
void cpu_get_brand(char out[49]);

#endif /* TUS_ARCH_CPU_H */
