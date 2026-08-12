/*
 * commands.h - built-in tsh command dispatch
 *
 * The command table lives in commands.c. Any subsystem that wants to
 * expose a shell command registers an entry there.
 */

#ifndef TUS_SHELL_COMMANDS_H
#define TUS_SHELL_COMMANDS_H

/* Tokenize a NUL-terminated line and run the matching command. */
void command_execute(const char *line);

#endif /* TUS_SHELL_COMMANDS_H */
