/*
 * commands.c - core tsh commands and dispatch
 *
 * Each command is a plain function `int fn(int argc, char **argv)`
 * with argv[0] being the command name itself, exactly like a UNIX
 * shell. The core commands live here; the file-system commands live
 * in cmd_fs.c (g_fs_commands) and are reached through the same table.
 */

#include "commands.h"

#include "tsh.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "core/bootinfo.h"
#include "core/console.h"
#include "core/klib.h"
#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "elf/tus_elf.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "vfs/vfs.h"

#define MAX_ARGS 16

static int cmd_help(int argc, char **argv);
static int cmd_clear(int argc, char **argv);
static int cmd_ver(int argc, char **argv);
static int cmd_about(int argc, char **argv);
static int cmd_sysinfo(int argc, char **argv);
static int cmd_reboot(int argc, char **argv);
static int cmd_crash(int argc, char **argv);

static const struct shell_command g_core_commands[] = {
    { "help",    "list available commands",      cmd_help },
    { "clear",   "clear the screen",             cmd_clear },
    { "ver",     "show the kernel version",      cmd_ver },
    { "about",   "show TUS information",         cmd_about },
    { "sysinfo", "show system information",      cmd_sysinfo },
    { "reboot",  "restart the machine",          cmd_reboot },
    { "crash",   "raise a CPU exception (demo)", cmd_crash },
};

#define CORE_COMMAND_COUNT (sizeof(g_core_commands) / sizeof(g_core_commands[0]))

static int cmd_help(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Available commands:\n");
    for (size_t i = 0; i < CORE_COMMAND_COUNT; i++) {
        kprintf("  %-10s %s\n", g_core_commands[i].name,
                g_core_commands[i].description);
    }
    for (size_t i = 0; i < g_fs_command_count; i++) {
        kprintf("  %-10s %s\n", g_fs_commands[i].name,
                g_fs_commands[i].description);
    }
    return 0;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_clear();
    return 0;
}

static int cmd_ver(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("TUS kernel 0.9.0, built with %s\n", __VERSION__);
    return 0;
}

static int cmd_about(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Toasty Unix Software (TUS)\n");
    console_write("\"Work everywhere, but work right.\"\n");
    console_write("Architecture: x86_64 (AMD64)\n");
    kprintf("Bootloader  : %s %s\n",
            g_bootinfo.bootloader_name ? g_bootinfo.bootloader_name : "unknown",
            g_bootinfo.bootloader_version ? g_bootinfo.bootloader_version : "");
    return 0;
}

static int cmd_sysinfo(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char vendor[13];
    char brand[49];
    cpu_get_vendor(vendor);
    cpu_get_brand(brand);
    kprintf("CPU vendor  : %s\n", vendor);
    kprintf("CPU model   : %s\n", brand);

    kprintf("Memory      : %llu MiB usable\n",
            (unsigned long long)(g_bootinfo.usable_memory_bytes / (1024 * 1024)));

    uint64_t total_frames = 0, free_frames = 0;
    pmm_get_stats(&total_frames, &free_frames);
    kprintf("PMM         : %llu free / %llu frames (%llu MiB)\n",
            (unsigned long long)free_frames, (unsigned long long)total_frames,
            (unsigned long long)(free_frames * 4 / 1024));

    kprintf("Uptime      : %llu.%03llu s\n",
            (unsigned long long)(pit_uptime_ms() / 1000),
            (unsigned long long)(pit_uptime_ms() % 1000));

    uint32_t width = 0, height = 0, bpp = 0;
    uint64_t pitch = 0;
    void *address = NULL;
    fb_get_info(&width, &height, &bpp, &pitch, &address);
    if (width > 0 && height > 0) {
        kprintf("Framebuffer : %ux%u, %u bpp, pitch %llu @ %p\n",
                width, height, bpp, (unsigned long long)pitch, address);
    } else {
        console_write("Framebuffer : none (serial console only)\n");
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Rebooting...\n");
    /* Ask the 8042 keyboard controller to pulse the reset line. */
    outb(0x64, 0xFE);
    /* If the reset never fires, stop here instead of continuing. */
    for (;;) {
        cli();
        hlt();
    }
    return 0;
}

static int cmd_crash(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Raising invalid opcode (#UD)...\n");
    __asm__ volatile("ud2"); /* never reached */
    return 0;
}

/*
 * tsh v2.0: pipes and I/O redirection.
 *
 * Operators (space-separated or glued, `2>` must touch):
 *
 *   |      pipeline   a | b | c   (stdout of a feeds stdin of b)
 *   >      stdout     cmd > file  (create/truncate)
 *   >>     stdout     cmd >> file (append)
 *   <      stdin      cmd < file
 *   2>     2>>        stderr redirection
 *
 * How it works: TUS keeps the fd table per task, and a spawned task
 * inherits a refcounted copy of the shell's table. The shell rewires
 * its OWN slots 0/1/2 (vfs_dup2), spawns the child, puts its own
 * slots back, and waits for the child's pid. External programs are
 * the real pipe users; builtins (echo writes through fd 1, so
 * `echo hi | grep hi` works) run inline with the same slot setup.
 * Commands that print through the console directly (help, ls, ...)
 * cannot be piped yet - only fd-1 writers can.
 */

#define MAX_PIPE_SEGS 8
#define MAX_TOKENS (TSH_LINE_MAX / 2 + 8)

struct pipeline_seg {
    char *argv[MAX_ARGS];
    int argc;
    int redir[3];   /* fd 0/1/2 redirected? */
    int rmode[3];   /* 0 = trunc, 1 = append, 2 = input */
    char *rfile[3];
    int builtin;    /* 0 = external program, else builtin_find() id */
};

/* Split a line into tokens, turning the operators into tokens of
 * their own. `2>x` merges into a single `2>` token (stderr). */
static int tokenize(char *s, char **tok, int max) {
    int n = 0;
    char *p = s;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (n >= max) {
            return -1;
        }
        if (*p == '|' || *p == '<' || *p == '>') {
            tok[n++] = p;
            char op = *p;
            p++;
            if (op == '>' && *p == '>') {
                p++;
            }
            if (*p != '\0') {
                *p = '\0';
                p++;
            }
            continue;
        }
        char *start = p;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '|' && *p != '<' && *p != '>') {
            p++;
        }
        if (*p) {
            /* Glued stderr redirection: `2>x` and `2>>x`. */
            if (*p == '>' && p == start + 1 && *start == '2') {
                p++;
                if (*p == '>') {
                    p++;
                }
            }
            if (*p != '\0') {
                *p = '\0';
                p++;
            }
        }
    }
    return n;
}

/* Turn the token stream into segments. Returns the segment count or
 * -1 on a syntax error (empty command, missing file after an
 * operator, too many stages). */
static int parse_pipeline(char **tok, int ntok, struct pipeline_seg *segs) {
    memset(segs, 0, sizeof(struct pipeline_seg) * MAX_PIPE_SEGS);
    int nseg = 0;
    struct pipeline_seg *cur = &segs[0];
    int t = 0;
    while (t < ntok) {
        const char *s = tok[t];
        if (strcmp(s, "|") == 0) {
            if (cur->argc == 0) {
                return -1;
            }
            if (++nseg >= MAX_PIPE_SEGS) {
                return -1;
            }
            cur = &segs[nseg];
            t++;
            continue;
        }
        int target = -1;
        int mode = -1;
        if (strcmp(s, ">") == 0) {
            target = 1;
            mode = 0;
        } else if (strcmp(s, ">>") == 0) {
            target = 1;
            mode = 1;
        } else if (strcmp(s, "<") == 0) {
            target = 0;
            mode = 2;
        } else if (strcmp(s, "2>") == 0) {
            target = 2;
            mode = 0;
        } else if (strcmp(s, "2>>") == 0) {
            target = 2;
            mode = 1;
        }
        if (target >= 0) {
            if (t + 1 >= ntok) {
                return -1; /* operator at the end: missing file */
            }
            cur->redir[target] = 1;
            cur->rmode[target] = mode;
            cur->rfile[target] = tok[t + 1];
            t += 2;
            continue;
        }
        if (cur->argc >= MAX_ARGS) {
            return -1;
        }
        cur->argv[cur->argc++] = tok[t];
        t++;
    }
    if (cur->argc == 0) {
        return -1; /* trailing | */
    }
    return nseg + 1;
}

/* Builtin lookup: > 0 = core table, < 0 = fs table, 0 = external. */
static int builtin_find(const char *name) {
    for (size_t i = 0; i < CORE_COMMAND_COUNT; i++) {
        if (strcmp(name, g_core_commands[i].name) == 0) {
            return (int)i + 1;
        }
    }
    for (size_t i = 0; i < g_fs_command_count; i++) {
        if (strcmp(name, g_fs_commands[i].name) == 0) {
            return -(int)i - 1;
        }
    }
    return 0;
}

static void builtin_run(int id, int argc, char **argv) {
    if (id > 0) {
        g_core_commands[id - 1].run(argc, argv);
    } else {
        g_fs_commands[-id - 1].run(argc, argv);
    }
}

/* Resolve a command name to a path: absolute paths pass through,
 * bare names are searched in /bin (executability = the x bit). */
static int resolve_prog(const char *name, char *out, size_t out_size) {
    if (strchr(name, '/') != NULL) {
        if (strlen(name) >= out_size) {
            return -1;
        }
        memcpy(out, name, strlen(name) + 1);
        return 0;
    }
    if (strlen(name) + 6 >= out_size) { /* "/bin/" + name */
        return -1;
    }
    memcpy(out, "/bin/", 5);
    memcpy(out + 5, name, strlen(name) + 1);
    struct vfs_node *node = vfs_lookup(out);
    if (node == NULL || node->type != VFS_FILE || (node->mode & 0111) == 0) {
        return -1;
    }
    return 0;
}

/* Run one pipeline. Every external stage is spawned as a task; the
 * shell waits (hlt) until all pids have exited. See the comment at
 * the top of this section for the fd choreography. */
static void exec_pipeline(struct pipeline_seg *segs, int nseg) {
    /* Pre-validate the external programs so a bad name aborts before
     * any task is spawned (and the shell's fds get tangled). */
    char resolved[MAX_PIPE_SEGS][160];
    for (int i = 0; i < nseg; i++) {
        if (segs[i].builtin == 0 &&
            resolve_prog(segs[i].argv[0], resolved[i], sizeof(resolved[i])) != 0) {
            kprintf("tsh: %s: command not found\n", segs[i].argv[0]);
            return;
        }
    }

    /* Save the standard descriptors (the shell's console) so the
     * shell's own slots can be restored after every segment. */
    long saves[3];
    saves[0] = vfs_dup(0);
    saves[1] = vfs_dup(1);
    saves[2] = vfs_dup(2);
    if (saves[0] < 0 || saves[1] < 0 || saves[2] < 0) {
        kprintf("tsh: too many open files\n");
        return;
    }

    int pids[MAX_PIPE_SEGS];
    int npids = 0;
    int prev_r = -1; /* read end of the pipe between the last two stages */
    int failed = 0;

    for (int i = 0; i < nseg; i++) {
        struct pipeline_seg *s = &segs[i];
        int w = -1;
        long filefds[3] = { -1, -1, -1 };

        if (!failed) {
            /* stdin: read end of the previous pipe (if any). */
            if (i > 0) {
                vfs_dup2(prev_r, 0);
                vfs_close(prev_r);
                prev_r = -1;
            }
            /* stdout: fresh pipe to the next stage (if any). */
            if (i < nseg - 1) {
                int pp[2];
                if (vfs_pipe(pp) != 0) {
                    failed = 1;
                } else {
                    vfs_dup2(pp[1], 1);
                    prev_r = pp[0];
                    w = pp[1];
                }
            }
            /* file redirections (> / >> / < / 2> / 2>>). */
            for (int tgt = 0; tgt < 3 && !failed; tgt++) {
                if (!s->redir[tgt]) {
                    continue;
                }
                int flags = s->rmode[tgt] == 2
                                ? O_RDONLY
                                : O_WRONLY | O_CREAT |
                                      (s->rmode[tgt] == 1 ? O_APPEND : O_TRUNC);
                filefds[tgt] = vfs_open(s->rfile[tgt], flags);
                if (filefds[tgt] < 0) {
                    kprintf("tsh: %s: cannot open\n", s->rfile[tgt]);
                    failed = 1;
                } else {
                    vfs_dup2(filefds[tgt], tgt);
                }
            }
            if (!failed) {
                if (s->builtin != 0) {
                    builtin_run(s->builtin, s->argc, s->argv);
                } else {
                    int pid = (int)elf_exec(resolved[i], s->argc - 1,
                                            &s->argv[1]);
                    if (pid < 0) {
                        kprintf("tsh: %s: cannot execute\n", s->argv[0]);
                        failed = 1;
                    } else {
                        pids[npids++] = pid;
                    }
                }
            }
        }

        /* The child (or inline builtin) is done with this setup: put
         * the shell's own descriptors back and drop the temporary
         * copies. The spawned task keeps its inherited table. */
        vfs_dup2(saves[0], 0);
        vfs_dup2(saves[1], 1);
        vfs_dup2(saves[2], 2);
        for (int tgt = 0; tgt < 3; tgt++) {
            if (filefds[tgt] >= 0) {
                vfs_close(filefds[tgt]);
            }
        }
        if (w >= 0) {
            vfs_close(w);
        }
        if (failed) {
            break;
        }
    }
    if (prev_r >= 0) {
        vfs_close(prev_r);
    }

    /* Wait for every stage: the shell idles in hlt, the PIT tick
     * preempts it and the children get CPU time. A pipe reader only
     * sees EOF once its writer has exited and closed its fds. */
    for (int k = 0; k < npids; k++) {
        while (sched_task_alive((uint32_t)pids[k])) {
            hlt();
        }
    }

    vfs_close(saves[0]);
    vfs_close(saves[1]);
    vfs_close(saves[2]);
}

/*
 * Tokenize the line into argv (space separated) and dispatch. The line
 * is copied first so the caller's buffer stays untouched. Lines with
 * pipe/redirection operators go to the pipeline executor; everything
 * else is a single command (builtin or external program).
 */
void command_execute(const char *line) {
    char buffer[TSH_LINE_MAX];
    char *tokens[MAX_TOKENS];
    struct pipeline_seg segs[MAX_PIPE_SEGS];

    size_t len = strlen(line);
    if (len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    memcpy(buffer, line, len);
    buffer[len] = '\0';

    int ntok = tokenize(buffer, tokens, MAX_TOKENS);
    if (ntok <= 0) {
        return; /* empty line */
    }

    /* Do any of the tokens look like an operator? */
    int has_ops = 0;
    for (int i = 0; i < ntok; i++) {
        const char *s = tokens[i];
        if (strcmp(s, "|") == 0 || strcmp(s, ">") == 0 ||
            strcmp(s, ">>") == 0 || strcmp(s, "<") == 0 ||
            strcmp(s, "2>") == 0 || strcmp(s, "2>>") == 0) {
            has_ops = 1;
            break;
        }
    }

    if (!has_ops) {
        /* Plain single command: builtin first, then the /bin PATH
         * lookup (a bare name becomes /bin/<name>; executability is
         * decided by the x permission bit, never an extension). */
        for (size_t i = 0; i < CORE_COMMAND_COUNT; i++) {
            if (strcmp(tokens[0], g_core_commands[i].name) == 0) {
                g_core_commands[i].run(ntok, tokens);
                return;
            }
        }
        for (size_t i = 0; i < g_fs_command_count; i++) {
            if (strcmp(tokens[0], g_fs_commands[i].name) == 0) {
                g_fs_commands[i].run(ntok, tokens);
                return;
            }
        }
        char pbin[128];
        if (resolve_prog(tokens[0], pbin, sizeof(pbin)) == 0) {
            /* A spawned task takes over the console keyboard while it
             * runs (ownership is claimed on first read, released in
             * task_exit); the shell waits for the task to exit. */
            memset(&segs[0], 0, sizeof(segs[0]));
            for (int i = 0; i < ntok && i < MAX_ARGS; i++) {
                segs[0].argv[i] = tokens[i];
            }
            segs[0].argc = ntok;
            segs[0].builtin = 0;
            exec_pipeline(segs, 1);
            return;
        }
        kprintf("tsh: %s: command not found\n", tokens[0]);
        return;
    }

    /* Operators present: parse into segments and run the pipeline. */
    int nseg = parse_pipeline(tokens, ntok, segs);
    if (nseg < 0) {
        kprintf("tsh: syntax error\n");
        return;
    }
    for (int i = 0; i < nseg; i++) {
        segs[i].builtin = builtin_find(segs[i].argv[0]);
    }
    exec_pipeline(segs, nseg);
}
