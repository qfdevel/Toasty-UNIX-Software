/*
 * commands.h - built-in tsh command dispatch
 *
 * The command table is split in two halves that share one layout:
 *   - g_core_commands (commands.c): kernel/service commands
 *   - g_fs_commands (cmd_fs.c): file/device/time commands, all of
 *     which go through the POSIX syscall ABI.
 */

#ifndef TUS_SHELL_COMMANDS_H
#define TUS_SHELL_COMMANDS_H

#include <stddef.h>

struct shell_command {
    const char *name;        /* what the user types */
    const char *description; /* shown by `help` */
    int (*run)(int argc, char **argv);
};

/* Commands defined in cmd_fs.c (file system via syscalls). */
extern const struct shell_command g_fs_commands[];
extern const size_t g_fs_command_count;

/* Current working directory of the shell (absolute, normalized).
 * Defined in cmd_fs.c; used by the prompt and by path resolution. */
const char *shell_cwd(void);

/* Tokenize a NUL-terminated line and run the matching command. */
void command_execute(const char *line);

#endif /* TUS_SHELL_COMMANDS_H */
