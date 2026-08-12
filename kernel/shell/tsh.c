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

/* TUS palette */
#define COLOR_FG     0x00E8E8E8 /* near-white text */
#define COLOR_BG     0x00000000 /* black background */
#define COLOR_ACCENT 0x00FFA040 /* warm toast orange */

static char g_line[TSH_LINE_MAX];
static int g_line_len;

/* Print the prompt in the accent color, then switch back. */
static void tsh_prompt(void) {
    console_set_color(COLOR_ACCENT, COLOR_BG);
    console_write("tus> ");
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
        struct kbd_event ev = kbd_get_event();

        /* PageUp/PageDown navigate the framebuffer scrollback. */
        if (ev.type == KBD_EVENT_SCROLL_UP) {
            console_scroll_page(1);
            continue;
        }
        if (ev.type == KBD_EVENT_SCROLL_DOWN) {
            console_scroll_page(-1);
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
