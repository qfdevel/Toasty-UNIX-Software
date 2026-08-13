/*
 * tus_syscall.c - TUS kernel ABI translation for musl
 *
 * TUS (Toasty Unix Software) exposes a compact POSIX-style syscall
 * ABI through `int $0x80` with its own numbering (kernel/syscall/
 * syscall.h). Upstream musl source uses Linux x86_64 syscall numbers
 * and the `syscall` instruction. This file is the bridge:
 *
 *   - __syscall0..6 (arch/x86_64/syscall_arch.h) forward here,
 *   - __syscall_cp_asm (src/thread/x86_64/syscall_cp.s) forwards
 *     here too (TUS has no thread cancellation yet),
 *
 * and it translates the Linux number to the TUS number, emulates a
 * few calls in userspace (converting timespecs, ignoring madvise/
 * umask/poll) and returns -ENOSYS (-38) for everything else, exactly
 * like an unknown Linux syscall.
 *
 * The kernel stub only restores RAX; every argument register is
 * clobbered by the trap, so all registers are declared read-write in
 * the asm (see TUS.md lesson 7/16).
 */

#include <stdint.h>

/* TUS syscall numbers (must match kernel/syscall/syscall.h). */
#define TUS_SYS_EXIT    0
#define TUS_SYS_READ    1
#define TUS_SYS_WRITE   2
#define TUS_SYS_OPEN    3
#define TUS_SYS_CLOSE   4
#define TUS_SYS_IOCTL   5
#define TUS_SYS_GETPID  6
#define TUS_SYS_SLEEP   8
#define TUS_SYS_MKDIR   9
#define TUS_SYS_UNLINK  10
#define TUS_SYS_MMAP    12
#define TUS_SYS_MUNMAP  13
#define TUS_SYS_ARCH_PRCTL 14
#define TUS_SYS_WRITEV  15
#define TUS_SYS_TIME    16
#define TUS_SYS_FTRUNCATE 17

#define TUS_ENOSYS (-38)
#define TUS_EFAULT (-14)

/* Userspace view of a struct timespec (fields match musl's layout). */
struct tus_timespec {
    long tv_sec;
    long tv_nsec;
};

/* The raw trap. Only RAX comes back; all argument registers are
 * read-write so the compiler reloads them around every call. */
static __inline long tus_raw(long n, long a1, long a2, long a3,
                             long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    register long rdi __asm__("rdi") = a1;
    register long rsi __asm__("rsi") = a2;
    register long rdx __asm__("rdx") = a3;
    __asm__ volatile("int $0x80"
                     : "=a"(ret), "+r"(rdi), "+r"(rsi), "+r"(rdx),
                       "+r"(r10), "+r"(r8), "+r"(r9)
                     : "0"(n)
                     : "rcx", "r11", "memory");
    return ret;
}

/* struct timeval (musl layout). */
struct tus_timeval {
    long tv_sec;
    long tv_usec;
};

/* clock_gettime / clock_gettime64 emulation: TUS has a single
 * monotonic clock (seconds since boot via SYS_TIME). REALTIME and
 * MONOTONIC both map to it. musl's time() is built on
 * clock_gettime(CLOCK_REALTIME), so without this every status
 * message in a TUS application would read uninitialised stack. */
static __inline long tus_clock_gettime(long clk, const void *ts) {
    if (ts == 0 || (clk != 0 && clk != 1)) {
        return -22; /* -EINVAL */
    }
    long secs = tus_raw(TUS_SYS_TIME, 0, 0, 0, 0, 0, 0);
    if (secs < 0) {
        return secs;
    }
    struct tus_timespec *t = (struct tus_timespec *)ts;
    t->tv_sec = secs;
    t->tv_nsec = 0;
    return 0;
}

/* Convert a userspace timespec to whole seconds, rounding up (the
 * TUS sleep syscall takes seconds). NULL maps to an immediate
 * return. */
static __inline long tus_sleep_from_timespec(const void *ts) {
    const struct tus_timespec *t = (const struct tus_timespec *)ts;
    if (t == 0) {
        return 0;
    }
    long secs = t->tv_sec;
    if (t->tv_nsec > 0) {
        secs += 1;
    }
    return tus_raw(TUS_SYS_SLEEP, secs, 0, 0, 0, 0, 0);
}

long tus_syscall(long n, long a1, long a2, long a3,
                 long a4, long a5, long a6) {
    switch (n) {
    /* Straight number translations (Linux x86_64 -> TUS). */
    case 0:   return tus_raw(TUS_SYS_READ, a1, a2, a3, 0, 0, 0);    /* read */
    case 1:   return tus_raw(TUS_SYS_WRITE, a1, a2, a3, 0, 0, 0);   /* write */
    case 2:   return tus_raw(TUS_SYS_OPEN, a1, a2, a3, 0, 0, 0);    /* open */
    case 3:   return tus_raw(TUS_SYS_CLOSE, a1, 0, 0, 0, 0, 0);     /* close */
    case 9:   return tus_raw(TUS_SYS_MMAP, a1, a2, a3, a4, 0, 0);   /* mmap */
    case 11:  return tus_raw(TUS_SYS_MUNMAP, a1, a2, 0, 0, 0, 0);   /* munmap */
    case 16:  return tus_raw(TUS_SYS_IOCTL, a1, a2, a3, 0, 0, 0);   /* ioctl */
    case 20:  return tus_raw(TUS_SYS_WRITEV, a1, a2, a3, 0, 0, 0);  /* writev */
    case 39:  return tus_raw(TUS_SYS_GETPID, 0, 0, 0, 0, 0, 0);     /* getpid */
    case 60:  return tus_raw(TUS_SYS_EXIT, a1, 0, 0, 0, 0, 0);      /* exit */
    case 77:  return tus_raw(TUS_SYS_FTRUNCATE, a1, a2, 0, 0, 0, 0); /* ftruncate */
    case 83:  return tus_raw(TUS_SYS_MKDIR, a1, 0, 0, 0, 0, 0);     /* mkdir */
    case 87:  return tus_raw(TUS_SYS_UNLINK, a1, 0, 0, 0, 0, 0);    /* unlink */
    case 158: return tus_raw(TUS_SYS_ARCH_PRCTL, a1, a2, 0, 0, 0, 0); /* arch_prctl */
    case 201: return tus_raw(TUS_SYS_TIME, 0, 0, 0, 0, 0, 0);       /* time */
    case 231: return tus_raw(TUS_SYS_EXIT, a1, 0, 0, 0, 0, 0);      /* exit_group */
    case 257: return tus_raw(TUS_SYS_OPEN, a2, a3, 0, 0, 0, 0);     /* openat(dirfd, path, flags, mode) */
    case 258: return tus_raw(TUS_SYS_MKDIR, a2, 0, 0, 0, 0, 0);     /* mkdirat(dirfd, path, mode) */
    case 263: return tus_raw(TUS_SYS_UNLINK, a2, 0, 0, 0, 0, 0);    /* unlinkat(dirfd, path, flags) */

    /* Emulated in userspace. */
    case 7:   return 0;   /* poll: report no events (stdin/out/err
                             become unbuffered, which suits a console) */
    case 28:  return 0;   /* madvise: allocator hint, safe to ignore */
    case 95:  return 0;   /* umask: TUS has no permission bits */
    case 35:  return tus_sleep_from_timespec((const void *)a1); /* nanosleep(req, rem) */
    case 96: { /* gettimeofday(tv, tz) */
        if (a1 == 0) {
            return 0;
        }
        long secs = tus_raw(TUS_SYS_TIME, 0, 0, 0, 0, 0, 0);
        if (secs < 0) {
            return secs;
        }
        struct tus_timeval *tv = (struct tus_timeval *)a1;
        tv->tv_sec = secs;
        tv->tv_usec = 0;
        return 0;
    }
    case 228: /* clock_gettime(clk, ts) */
        return tus_clock_gettime(a1, (const void *)a2);
    case 403: /* clock_gettime64(clk, ts) */
        return tus_clock_gettime(a1, (const void *)a2);
    case 115: /* clock_nanosleep_time64(clk, flags, req, rem) */
    case 230: /* clock_nanosleep(clk, flags, req, rem) */
        if (a2 != 0) {
            return TUS_EFAULT; /* no TIMER_ABSTIME support */
        }
        return tus_sleep_from_timespec((const void *)a3);

    /* Not implemented by TUS yet: behave like an unknown syscall. */
    default:
        return TUS_ENOSYS;
    }
}
