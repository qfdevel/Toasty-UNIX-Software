/*
 * enforce.elf - verifies ring-3 syscall pointer enforcement
 *
 * A user program may only pass pointers into the canonical user
 * half. Passing a kernel address to write() must fail with -EFAULT
 * (-14) instead of letting the kernel write to kernel memory. This
 * program prints the syscall return value so the boot test can
 * assert the exact errno.
 *
 * Build (from tests/):
 *   gcc -m64 -ffreestanding -fno-stack-protector -fno-pic \
 *       -mno-red-zone -mgeneral-regs-only -O2 -c enforce.c -o enforce.o
 *   ld -m elf_x86_64 -static -e _start -Ttext 0x10000000 \
 *       -o enforce.elf enforce.o
 */

/* TUS syscall wrapper. The kernel stub does NOT restore the argument
 * registers (only RAX comes back), so every register is declared
 * read-write ("+r") - GCC must assume they are clobbered and reload
 * them before each call (see TUS.md lesson 7). */
static long tus_syscall(long number, long a1, long a2, long a3,
                        long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = 0;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    __asm__ volatile("int $0x80"
                     : "=a"(ret), "+r"(rdi), "+r"(rsi), "+r"(rdx),
                       "+r"(r10), "+r"(r8), "+r"(r9)
                     : "a"(number)
                     : "rcx", "r11", "memory");
    return ret;
}

/* write(1, buf, len): tiny stdout helper (no libc). */
static void print_str(const char *s, long len) {
    tus_syscall(2, 1, (long)s, len, 0, 0);
}

/* Convert a long to decimal and write it, followed by a newline. */
static void print_long(long v) {
    char out[24];
    int i = 0;
    char tmp[20];
    int n = 0;

    if (v < 0) {
        out[i++] = '-';
        v = -v;
    }
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0) {
        out[i++] = tmp[--n];
    }
    out[i++] = '\n';
    print_str(out, i);
}

void _start(void) {
    /* write(1, <kernel heap address>, 8) from ring 3 must fail with
     * -EFAULT instead of scribbling over kernel memory. */
    long r = tus_syscall(2, 1, 0xffffffff81000000ull, 8, 0, 0);
    print_long(r);
    tus_syscall(0, 0, 0, 0, 0, 0); /* exit(0) */
}
