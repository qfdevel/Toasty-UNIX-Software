/*
 * hello.elf - test program for the TUS ELF loader
 *
 * A tiny static x86-64 binary with no libc: _start is the entry
 * point. It exercises the TUS syscall ABI (int $0x80) the same way
 * the kernel shell does, then returns to the caller (the loader).
 *
 * Build (from tests/):
 *   gcc -m64 -ffreestanding -fno-stack-protector -fno-pic \
 *       -mno-red-zone -mgeneral-regs-only -O2 -c hello.c -o hello.o
 *   ld -m elf_x86_64 -static -e _start -Ttext 0x400000 \
 *       -o hello.elf hello.o
 */

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
                     : "=a"(ret)
                     : "a"(number), "r"(rdi), "r"(rsi), "r"(rdx),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* write(1, msg, len) then return to the loader (no exit: there are
 * no processes yet, so SYS_EXIT would halt the whole system). */
void _start(void) {
    const char *msg = "Hello from a static ELF on TUS!\n";
    tus_syscall(2, 1, (long)msg, 31, 0, 0); /* SYS_WRITE */
}
