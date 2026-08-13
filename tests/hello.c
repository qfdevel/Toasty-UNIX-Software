/*
 * hello.elf - test program for the TUS scheduler + ELF loader
 *
 * A tiny static x86-64 binary with no libc: _start is the entry
 * point. It runs in ring 3, exercises the TUS syscall ABI (int $0x80)
 * and terminates itself with SYS_EXIT - the scheduler then switches
 * back to the shell. The message length is baked in; a framebuffer
 * write would need an ioctl, so this only uses the text console.
 *
 * Build (from tests/):
 *   gcc -m64 -ffreestanding -fno-stack-protector -fno-pic \
 *       -mno-red-zone -mgeneral-regs-only -O2 -c hello.c -o hello.o
 *   ld -m elf_x86_64 -static -e _start -Ttext 0x10000000 \
 *       -o hello.elf hello.o
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

/* write(1, msg, len) then exit(0) */
void _start(void) {
    const char *msg = "Hello from a static ELF in ring 3!\n";
    tus_syscall(2, 1, (long)msg, 35, 0, 0); /* SYS_WRITE */
    tus_syscall(0, 0, 0, 0, 0, 0);          /* SYS_EXIT */
}
