/*
 * tsh.h - TUS shell
 *
 * tsh is the interactive command line of TUS. It reads characters from
 * the keyboard driver, maintains a small line editor (backspace,
 * Ctrl+L to clear) and dispatches completed lines to the command
 * table. A userspace shell will replace it once the kernel can run
 * user processes.
 */

#ifndef TUS_SHELL_TSH_H
#define TUS_SHELL_TSH_H

/* Maximum length of a single command line, including the NUL byte. */
#define TSH_LINE_MAX 128

/* Run the shell; never returns. */
void tsh_run(void);

#endif /* TUS_SHELL_TSH_H */
