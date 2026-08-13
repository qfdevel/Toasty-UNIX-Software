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
#include "../drivers/keyboard.h"
#include "../elf/tus_elf.h"
#include "../sched/sched.h"
#include "../syscall/syscall.h"
#include "../vfs/devices.h"
#include "../vfs/vfs.h"

/* ---- helpers ---- */

/* Current working directory of the shell. The VFS itself only knows
 * absolute paths; the shell resolves every relative path against
 * this before calling the syscall ABI (a real UNIX shell would hand
 * the kernel a relative path and let it resolve - our VFS is still
 * absolute-only, so the shell does the resolution). */
static char g_cwd[128] = "/";

/* Longest normalized path we hand to the syscall ABI. */
#define PATH_BUF 256

const char *shell_cwd(void) {
    return g_cwd;
}

/* Resolve `in` (absolute or relative) against g_cwd into `out`:
 * collapses duplicate slashes, honors "." and ".." (".." at the
 * root stays at the root). `out` always receives a normalized
 * absolute path. */
static void path_resolve(const char *in, char *out, size_t outsz) {
    char tmp[PATH_BUF + 32];

    if (in[0] == '/') {
        strncpy(tmp, in, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else {
        size_t cl = strlen(g_cwd);
        size_t il = strlen(in);
        if (cl + 1 + il >= sizeof(tmp)) {
            strncpy(out, "/", outsz); /* absurdly long: fall back to root */
            return;
        }
        memcpy(tmp, g_cwd, cl);
        tmp[cl] = '/';
        memcpy(tmp + cl + 1, in, il + 1);
    }

    /* Split on '/', honoring "." and "..". */
    const char *segs[32];
    size_t lens[32];
    int n = 0;

    const char *p = tmp;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *s = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - s);

        if (len == 1 && s[0] == '.') {
            continue;
        }
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (n > 0) {
                n--; /* walk up one level */
            }
            continue;
        }
        if (n < 32) {
            segs[n] = s;
            lens[n] = len;
            n++;
        }
    }

    /* Rebuild the normalized absolute path. */
    char *w = out;
    size_t left = outsz;
    *w++ = '/';
    left--;
    for (int i = 0; i < n && left > 1; i++) {
        if (i > 0) {
            *w++ = '/';
            left--;
        }
        size_t l = lens[i] < left - 1 ? lens[i] : left - 1;
        memcpy(w, segs[i], l);
        w += l;
        left -= l;
    }
    *w = '\0';
}

static void print_syscall_error(const char *what, long err) {
    kprintf("%s: error %ld\n", what, -err);
}

/* ---- pwd / cd ---- */

static int cmd_pwd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("%s\n", g_cwd);
    return 0;
}

static int cmd_cd(int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "/";

    char resolved[PATH_BUF];
    path_resolve(target, resolved, sizeof(resolved));

    /* Only enter real directories: open + readdir succeeds for dirs,
     * returns -ENOTDIR for files. */
    long fd = syscall(SYS_OPEN, (long)resolved, O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        print_syscall_error("cd", fd);
        return 1;
    }
    struct vfs_dirent ent;
    long r = syscall(SYS_READDIR, fd, (long)&ent, sizeof(ent), 0, 0);
    syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
    if (r < 0) {
        kprintf("cd: not a directory: %s\n", resolved);
        return 1;
    }

    strncpy(g_cwd, resolved, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
    return 0;
}

/* ---- ls ---- */

static int cmd_ls(int argc, char **argv) {
    char resolved[PATH_BUF];
    if (argc > 1) {
        path_resolve(argv[1], resolved, sizeof(resolved));
    } else {
        strncpy(resolved, g_cwd, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    long fd = syscall(SYS_OPEN, (long)resolved, O_RDONLY, 0, 0, 0);
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

    char resolved[PATH_BUF];
    path_resolve(argv[1], resolved, sizeof(resolved));

    long fd = syscall(SYS_OPEN, (long)resolved, O_RDONLY, 0, 0, 0);
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
    char resolved[PATH_BUF];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0 && i + 1 < argc) {
            path_resolve(argv[i + 1], resolved, sizeof(resolved));
            fd = syscall(SYS_OPEN, (long)resolved,
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
    char resolved[PATH_BUF];
    path_resolve(argv[1], resolved, sizeof(resolved));
    long r = syscall(SYS_MKDIR, (long)resolved, 0, 0, 0, 0);
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
    char resolved[PATH_BUF];
    path_resolve(argv[1], resolved, sizeof(resolved));
    long fd = syscall(SYS_OPEN, (long)resolved, O_CREAT | O_RDWR, 0, 0, 0);
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
    char resolved[PATH_BUF];
    path_resolve(argv[1], resolved, sizeof(resolved));
    long r = syscall(SYS_UNLINK, (long)resolved, 0, 0, 0, 0);
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

/* ---- exec: run a static ELF image ---- */

static int cmd_exec(int argc, char **argv) {
    if (argc < 2) {
        console_write("usage: exec <static-elf-path> [args...]\n");
        return 1;
    }
    char resolved[PATH_BUF];
    path_resolve(argv[1], resolved, sizeof(resolved));
    /* Hand the console keyboard to the new program: it may be a
     * foreground application (kilo) that wants the tty in raw mode.
     * The new task claims ownership on its first read and gives it
     * back when it exits. */
    kbd_input_release(kbd_input_owner());
    elf_exec(resolved, argc - 2, &argv[2]);
    return 0;
}

/* ---- ps: list tasks ---- */

static int cmd_ps(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("PID  STATE   CR3       NAME\n");
    task_list_all();
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
    { "cd",      "change the working directory", cmd_cd },
    { "pwd",     "print the working directory", cmd_pwd },
    { "uptime",  "time since boot",             cmd_uptime },
    { "sleep",   "wait N milliseconds",         cmd_sleep },
    { "fbfill",  "fill the framebuffer with a color", cmd_fbfill },
    { "exec",    "run a static ELF binary",     cmd_exec },
    { "ps",      "list running tasks",          cmd_ps },
};

const size_t g_fs_command_count =
    sizeof(g_fs_commands) / sizeof(g_fs_commands[0]);
