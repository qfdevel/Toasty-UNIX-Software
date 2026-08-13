/*
 * echo.c - print its arguments to stdout, one space between, newline
 * at the end (TUS port of the classic UNIX echo).
 *
 * Useful both standalone and as the first stage of a pipeline:
 * `echo hello world | grep hello` - it writes through fd 1, so the
 * pipe machinery (SYS_WRITE on the inherited pipe fd) just works.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            write(1, " ", 1);
        }
        write(1, argv[i], strlen(argv[i]));
    }
    write(1, "\n", 1);
    return 0;
}
