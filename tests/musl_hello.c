/*
 * musl_hello.c - first TUS userspace program linked against the
 * ported musl C library (musl-1.2.6).
 *
 * This exercises the whole TUS libc plumbing from ring 3:
 *   - printf / fprintf  -> stdio -> writev / open / close
 *   - malloc / free     -> mallocng -> mmap / munmap / madvise
 *   - strlen            -> SSE2 asm in libc (needs FPU context
 *                          switching in the scheduler)
 *   - getpid            -> syscall ABI
 *   - errno, TLS        -> arch_prctl(ARCH_SET_FS) thread pointer
 *
 * Build (see Makefile): compile against the musl headers, link
 * statically with crt1.o/crti.o/libc.a/crtn.o at 0x10000000.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("musl 1.2.6 on TUS: hello from libc\n");
    printf("argc=%d argv0=%s\n", argc, argv[0] ? argv[0] : "(null)");
    printf("pid=%d\n", getpid());

    char *buf = malloc(128);
    if (buf == NULL) {
        printf("malloc failed\n");
        return 1;
    }
    strcpy(buf, "heap string");
    printf("malloc: %s (%zu bytes, strlen=%zu)\n", buf,
           (size_t)128, strlen(buf));
    free(buf);
    printf("free ok\n");

    FILE *f = fopen("/tmp/musl.txt", "w");
    if (f == NULL) {
        printf("fopen failed\n");
        return 1;
    }
    fprintf(f, "written by musl fopen/fprintf\n");
    fclose(f);

    printf("all good\n");
    return 0;
}
