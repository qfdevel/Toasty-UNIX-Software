/*
 * TUS syscall interface for x86_64 (modified from upstream musl).
 *
 * Upstream musl invokes the Linux kernel with the `syscall`
 * instruction and Linux syscall numbers. TUS instead traps through
 * `int $0x80` with its own compact POSIX-style ABI (see
 * kernel/syscall/syscall.h in the TOS tree):
 *
 *     RAX = TUS syscall number, RDI/RSI/RDX/R10/R8 = up to 5 args
 *     RAX = result (>= 0 or negative errno; only RAX is preserved)
 *
 * Every call is routed through tus_syscall() (src/internal/
 * tus_syscall.c), which translates the Linux syscall number used by
 * the musl source to the matching TUS number, emulates a few calls
 * in userspace (nanosleep, madvise, poll, umask) and returns -ENOSYS
 * for anything TUS does not implement - the same behaviour a program
 * would see for an unknown Linux syscall.
 */

#ifndef _X86_64_SYSCALL_ARCH_H
#define _X86_64_SYSCALL_ARCH_H

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

/* Implemented in src/internal/tus_syscall.c. */
long tus_syscall(long n, long a1, long a2, long a3,
                 long a4, long a5, long a6);

static __inline long __syscall0(long n)
{
	return tus_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
	return tus_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return tus_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return tus_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return tus_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return tus_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return tus_syscall(n, a1, a2, a3, a4, a5, a6);
}

/* TUS has no vDSO; clock_gettime falls back to the syscall path. */

#define IPC_64 0

#endif /* _X86_64_SYSCALL_ARCH_H */
