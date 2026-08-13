/*
 * tsh.c - TUS shell implementation
 *
 * The shell loop blocks in kbd_getchar(), which halts the CPU until
 * the keyboard interrupt wakes it - no busy waiting, no scheduler yet.
 */

#include "tsh.h"

#include "commands.h"
#include "../core/console.h"
#include "../drivers/keyboard.h"
#include "../sched/sched.h"
#include "../vfs/devices.h"

/* TUS palette */
#define COLOR_FG     0x00E8E8E8 /* near-white text */
#define COLOR_BG     0x00000000 /* black background */
#define COLOR_ACCENT 0x00FFA040 /* warm toast orange */

static char g_line[TSH_LINE_MAX];
static int g_line_len;

/* Print the prompt in the accent color, then switch back. The
 * prompt shows the shell's working directory, like a real UNIX
 * shell: tus:/tmp>  (the directory part comes from cmd_fs.c). */
static void tsh_prompt(void) {
    console_set_color(COLOR_ACCENT, COLOR_BG);
    console_write("tus:");
    console_write(shell_cwd());
    console_write("> ");
    console_set_color(COLOR_FG, COLOR_BG);
}

/* Execute the accumulated line and reset the editor. */
static void tsh_process_line(void) {
    g_line[g_line_len] = '\0';
    command_execute(g_line);
    g_line_len = 0;
}

void tsh_run(void) {
    tsh_prompt();

    for (;;) {
        /* A foreground user task (kilo and friends) owns the console
         * keyboard while it runs; kbd_get_event_owned() yields until
         * ownership comes back (the owner releases it in task_exit). */
        struct task *me = sched_current();
        long pid = me != NULL ? me->pid : 1;
        struct kbd_event ev = kbd_get_event_shell(pid);

        /* PageUp/PageDown navigate the framebuffer scrollback. */
        if (ev.type == KBD_EVENT_SPECIAL) {
            if (ev.code == KBD_KEY_PAGE_UP) {
                console_scroll_page(1);
                continue;
            }
            if (ev.code == KBD_KEY_PAGE_DOWN) {
                console_scroll_page(-1);
                continue;
            }
            continue; /* other special keys are ignored by the shell */
        }
        if (ev.type != KBD_EVENT_CHAR) {
            continue;
        }

        char c = ev.c;

        if (c == '\n') {
            console_putchar('\n');
            tsh_process_line();
            tsh_prompt();
        } else if (c == '\b') {
            if (g_line_len > 0) {
                g_line_len--;
                console_putchar('\b');
            }
        } else if (c == 0x0C) { /* Ctrl+L: clear the screen */
            console_clear();
            tsh_prompt();
        } else if (c >= 0x20 && c < 0x7F) {
            if (g_line_len < TSH_LINE_MAX - 1) {
                g_line[g_line_len++] = c;
                console_putchar(c);
            }
        }
        /* any other control character is ignored */
    }
}
