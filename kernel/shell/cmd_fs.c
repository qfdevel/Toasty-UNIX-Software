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
static char g_oldpwd[128] = ""; /* previous directory (cd -) */

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
    const char *target = "/";
    bool print_target = false;

    if (argc > 1) {
        if (strcmp(argv[1], "-") == 0) {
            /* cd - : go to the previous directory and print it. */
            if (g_oldpwd[0] == '\0') {
                kprintf("cd: no previous directory\n");
                return 1;
            }
            target = g_oldpwd;
            print_target = true;
        } else if (strcmp(argv[1], "~") == 0) {
            target = "/"; /* HOME; TUS has no per-user homes yet */
        } else {
            target = argv[1];
        }
    }

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

    strncpy(g_oldpwd, g_cwd, sizeof(g_oldpwd) - 1);
    g_oldpwd[sizeof(g_oldpwd) - 1] = '\0';
    strncpy(g_cwd, resolved, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
    if (print_target) {
        kprintf("%s\n", resolved);
    }
    return 0;
}

/* ---- ls ---- */

/* ---- ls ---- */

/* Format the permission bits the way ls -l does: -rwxr-xr-x, with
 * 's'/'S' for setuid/setgid and 't'/'T' for the sticky bit. */
static void mode_string(uint32_t mode, uint32_t type, char out[11]) {
    out[0] = (type == VFS_DIR) ? 'd' : (type == VFS_DEVICE) ? 'c' : '-';
    static const char rwx[10] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) {
        out[1 + i] = (mode & (0400u >> i)) ? rwx[i] : '-';
    }
    if (mode & 04000) {
        out[3] = (out[3] == 'x') ? 's' : 'S';
    }
    if (mode & 02000) {
        out[6] = (out[6] == 'x') ? 's' : 'S';
    }
    if (mode & 01000) {
        out[9] = (out[9] == 'x') ? 't' : 'T';
    }
    out[10] = '\0';
}

struct ls_entry {
    char name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
    uint32_t mode;
};

static int cmd_ls(int argc, char **argv) {
    bool long_fmt = false;
    bool all = false;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (const char *p = argv[i] + 1; *p != '\0'; p++) {
                if (*p == 'l') {
                    long_fmt = true;
                } else if (*p == 'a') {
                    all = true;
                } else {
                    kprintf("ls: invalid option -- '%c'\n", *p);
                    return 1;
                }
            }
        } else if (target == NULL) {
            target = argv[i];
        }
    }

    char resolved[PATH_BUF];
    if (target != NULL) {
        path_resolve(target, resolved, sizeof(resolved));
    } else {
        strncpy(resolved, g_cwd, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    long fd = syscall(SYS_OPEN, (long)resolved, O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        print_syscall_error("ls", fd);
        return 1;
    }

    /* Collect and sort the entries (UNIX ls sorts by name). */
    struct ls_entry ents[128];
    int count = 0;
    struct vfs_dirent ent;
    long n;
    while ((n = syscall(SYS_READDIR, fd, (long)&ent, sizeof(ent), 0, 0)) > 0
           && count < 128) {
        if (!all && ent.name[0] == '.') {
            continue;
        }
        strncpy(ents[count].name, ent.name, VFS_NAME_MAX - 1);
        ents[count].name[VFS_NAME_MAX - 1] = '\0';
        ents[count].type = ent.type;
        ents[count].size = ent.size;
        ents[count].mode = ent.mode;
        count++;
    }
    if (n < 0) {
        print_syscall_error("ls", n);
    }
    syscall(SYS_CLOSE, fd, 0, 0, 0, 0);

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(ents[j].name, ents[i].name) < 0) {
                struct ls_entry tmp = ents[i];
                ents[i] = ents[j];
                ents[j] = tmp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        if (long_fmt) {
            char m[11];
            mode_string(ents[i].mode, ents[i].type, m);
            const char *kind = (ents[i].type == VFS_DIR) ? "/"
                             : (ents[i].type == VFS_DEVICE) ? "" : "";
            kprintf("%s root root %8u %s%s\n", m, ents[i].size,
                    ents[i].name, kind);
        } else {
            kprintf("%s\n", ents[i].name);
        }
    }
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
    bool parents = false;
    bool verbose = false;
    uint32_t mode = 0;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0' && target == NULL) {
            for (const char *p = argv[i] + 1; *p != '\0'; p++) {
                if (*p == 'p') {
                    parents = true;
                } else if (*p == 'v') {
                    verbose = true;
                } else if (*p == 'm') {
                    if (i + 1 < argc) {
                        mode = (uint32_t)strtoul(argv[++i], NULL, 8);
                    } else {
                        kprintf("mkdir: option requires an argument -- 'm'\n");
                        return 1;
                    }
                } else {
                    kprintf("mkdir: invalid option -- '%c'\n", *p);
                    return 1;
                }
            }
        } else if (target == NULL) {
            target = argv[i];
        }
    }
    if (target == NULL) {
        console_write("usage: mkdir [-p] [-v] [-m mode] <directory>\n");
        return 1;
    }

    char resolved[PATH_BUF];
    path_resolve(target, resolved, sizeof(resolved));

    long r;
    if (parents) {
        /* Create every missing component of the path. */
        char comp[PATH_BUF];
        size_t len = strlen(resolved);
        r = 0;
        for (size_t i = 1; i <= len; i++) {
            if (i == len || resolved[i] == '/') {
                size_t n = i;
                if (i == len && n > 1 && resolved[n - 1] == '/') {
                    n--;
                }
                memcpy(comp, resolved, n);
                comp[n] = '\0';
                if (n > 1) {
                    long rr = syscall(SYS_MKDIR, (long)comp, mode, 0, 0, 0);
                    if (rr < 0 && rr != -17 /* EEXIST */) {
                        r = rr;
                        break;
                    } else if (verbose && rr == 0) {
                        kprintf("mkdir: created directory '%s'\n", comp);
                    }
                }
            }
        }
    } else {
        r = syscall(SYS_MKDIR, (long)resolved, mode, 0, 0, 0);
        if (r == 0 && verbose) {
            kprintf("mkdir: created directory '%s'\n", resolved);
        }
    }
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
