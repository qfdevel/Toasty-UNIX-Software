/*
 * cmd_fs.c - tsh commands for files, devices and time
 *
 * Every command in this file talks to the kernel through the POSIX
 * syscall ABI (see syscall.h) instead of calling kernel functions
 * directly. That keeps the shell honest as the first "program" that
 * exercises the system call interface, and every one of these
 * commands will keep working unchanged once user processes exist.
 */

#include "commands.h"

#include "tsh.h"
#include "../core/console.h"
#include "../core/klib.h"
#include "../syscall/syscall.h"
#include "../vfs/devices.h"
#include "../vfs/vfs.h"

/* ---- helpers ---- */

static void print_syscall_error(const char *what, long err) {
    kprintf("%s: error %ld\n", what, -err);
}

/* ---- ls ---- */

static int cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";

    long fd = syscall(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        print_syscall_error("ls", fd);
        return 1;
    }

    struct vfs_dirent ent;
    long n;
    while ((n = syscall(SYS_READDIR, fd, (long)&ent, sizeof(ent), 0, 0)) > 0) {
        const char *kind = (ent.type == VFS_DIR) ? "dir "
                         : (ent.type == VFS_DEVICE) ? "dev "
                         : "file";
        kprintf("%-16s %s %8u\n", ent.name, kind, ent.size);
    }
    if (n < 0) {
        print_syscall_error("ls", n);
    }
    syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
    return 0;
}

/* ---- cat ---- */

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: cat <path>\n");
        return 1;
    }

    long fd = syscall(SYS_OPEN, (long)argv[1], O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        print_syscall_error("cat", fd);
        return 1;
    }

    char buf[256];
    long n;
    while ((n = syscall(SYS_READ, fd, (long)buf, sizeof(buf), 0, 0)) > 0) {
        syscall(SYS_WRITE, 1, (long)buf, n, 0, 0);
    }
    if (n < 0) {
        print_syscall_error("cat", n);
    }
    syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
    return 0;
}

/* ---- echo (with `> file` redirection) ---- */

static int cmd_echo(int argc, char **argv) {
    /* Detect a ">" redirection: echo a b > path */
    long fd = 1; /* stdout by default */
    int end_arg = argc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0 && i + 1 < argc) {
            fd = syscall(SYS_OPEN, (long)argv[i + 1],
                         O_WRONLY | O_CREAT | O_TRUNC, 0, 0, 0);
            if (fd < 0) {
                print_syscall_error("echo", fd);
                return 1;
            }
            end_arg = i;
            break;
        }
    }

    for (int i = 1; i < end_arg; i++) {
        if (i > 1) {
            syscall(SYS_WRITE, fd, (long)" ", 1, 0, 0);
        }
        syscall(SYS_WRITE, fd, (long)argv[i], (long)strlen(argv[i]), 0, 0);
    }
    syscall(SYS_WRITE, fd, (long)"\n", 1, 0, 0);

    if (fd != 1) {
        syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
    }
    return 0;
}

/* ---- mkdir / touch / rm ---- */

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: mkdir <path>\n");
        return 1;
    }
    long r = syscall(SYS_MKDIR, (long)argv[1], 0, 0, 0, 0);
    if (r < 0) {
        print_syscall_error("mkdir", r);
        return 1;
    }
    return 0;
}

static int cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: touch <path>\n");
        return 1;
    }
    long fd = syscall(SYS_OPEN, (long)argv[1], O_CREAT | O_RDWR, 0, 0, 0);
    if (fd < 0) {
        print_syscall_error("touch", fd);
        return 1;
    }
    syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: rm <path>\n");
        return 1;
    }
    long r = syscall(SYS_UNLINK, (long)argv[1], 0, 0, 0, 0);
    if (r < 0) {
        print_syscall_error("rm", r);
        return 1;
    }
    return 0;
}

/* ---- uptime / sleep ---- */

static int cmd_uptime(int argc, char **argv) {
    (void)argc;
    (void)argv;
    long ms = syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
    kprintf("uptime: %ld.%03ld s\n", ms / 1000, ms % 1000);
    return 0;
}

static int cmd_sleep(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: sleep <milliseconds>\n");
        return 1;
    }
    syscall(SYS_SLEEP, (long)strtoul(argv[1], NULL, 10), 0, 0, 0, 0);
    return 0;
}

/* ---- fbfill: paint the whole framebuffer via an ioctl ---- */

static int cmd_fbfill(int argc, char **argv) {
    uint32_t color = 0xFFFFFF; /* default: white */
    if (argc > 1) {
        color = (uint32_t)strtoul(argv[1], NULL, 16) & 0xFFFFFF;
    }

    long fd = syscall(SYS_OPEN, (long)"/dev/fb0", O_WRONLY, 0, 0, 0);
    if (fd < 0) {
        print_syscall_error("fbfill", fd);
        return 1;
    }
    long r = syscall(SYS_IOCTL, fd, FB_IOCTL_FILL, (long)&color, 0, 0);
    if (r < 0) {
        print_syscall_error("fbfill", r);
    } else {
        kprintf("fb0: filled with #%06x\n", color);
    }
    syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
    return 0;
}

/* Command table additions, referenced from commands.c. */
const struct shell_command g_fs_commands[] = {
    { "ls",      "list a directory",            cmd_ls },
    { "cat",     "print a file or device",      cmd_cat },
    { "echo",    "print text (supports > file)", cmd_echo },
    { "mkdir",   "create a directory",          cmd_mkdir },
    { "touch",   "create an empty file",        cmd_touch },
    { "rm",      "remove a file",               cmd_rm },
    { "uptime",  "time since boot",             cmd_uptime },
    { "sleep",   "wait N milliseconds",         cmd_sleep },
    { "fbfill",  "fill the framebuffer with a color", cmd_fbfill },
};

const size_t g_fs_command_count =
    sizeof(g_fs_commands) / sizeof(g_fs_commands[0]);
