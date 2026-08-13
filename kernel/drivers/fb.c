/*
 * fb.c - framebuffer text console implementation
 *
 * A full-screen text buffer (g_text) mirrors what is on screen. Each
 * character cell is redrawn on demand, which makes cursor movement and
 * editing cheap; scrolling shifts both the text buffer and the pixel
 * rows. The cursor is drawn as an inverted block.
 *
 * Only 32-bit RGB framebuffers are supported (the Limine default);
 * a pitch of width*4 bytes is expected but any pitch works.
 */

#include "fb.h"

#include <stdbool.h>

#include "font8x16.h"
#include "core/klib.h"

/* Grid size caps; the real grid is clamped to these. */
#define FB_MAX_COLS 512
#define FB_MAX_ROWS 256

/* Scrollback history: completed lines are kept here so the user can
 * page back with PageUp/PageDown. Ring buffer: g_hist_head is the
 * next slot to write, g_hist_count the number of lines stored. */
#define FB_HISTORY_ROWS 2048

/* One screen cell: character + palette index for its color pair.
 * Colors are stored as indices into a small palette (see
 * fb_set_color) so a scrollback redraw reproduces the exact colors
 * the text had when it was written. */
struct fb_cell {
    char c;
    uint8_t color;
};

/* Palette of color pairs; index 0 is the default (near-white on
 * black). fb_set_color looks up or appends the requested pair. */
#define FB_PALETTE_SIZE 16

/* Default palette: near-white on black, warm toast accent for prompts. */
#define COLOR_FG 0x00E8E8E8
#define COLOR_BG 0x00000000

static struct limine_framebuffer *g_fb;
static uint32_t *g_pixels;
static uint64_t g_pitch_bytes;
static uint64_t g_pitch_words; /* pitch divided by 4 */
static uint32_t g_width;
static uint32_t g_height;

/* Pixel row where the text grid starts. The boot splash draws logos
 * in the band above this offset; text rendering, scrolling and the
 * scrollback redraw all work below it. fb_clear() resets it to 0. */
static uint32_t g_text_top;

static int g_cols;
static int g_rows;
static int g_cursor_x;
static int g_cursor_y;

static uint32_t g_fg = COLOR_FG;
static uint32_t g_bg = COLOR_BG;
static uint8_t g_cur_color; /* palette index of the current colors */

static uint32_t g_palette_fg[FB_PALETTE_SIZE];
static uint32_t g_palette_bg[FB_PALETTE_SIZE];
static int g_palette_count;

static struct fb_cell g_text[FB_MAX_ROWS][FB_MAX_COLS];

/* Forward declarations: the helpers live below, the ANSI engine
 * (inserted right after the globals) calls them. */
static uint8_t palette_lookup(uint32_t fg, uint32_t bg);
static void fb_paint_cell(int row, int col, const struct fb_cell *cell,
                          bool show_cursor);
static void fb_draw_cell(int row, int col, bool show_cursor);
static void fb_redraw_live(void);

/* ---- ANSI/VT100 escape sequence state ----
 *
 * The console understands the subset of ECMA-48 / VT100 sequences a
 * full-screen terminal application (kilo) needs: CUP cursor
 * positioning (H/f), relative moves (A/B/C/D), erase display/line
 * (J/K/X), SGR colours (m, incl. reverse video 7 and the 30-37 /
 * 90-97 / 40-47 / 100-107 colour sets), cursor visibility (?25h/l)
 * and the alternate screen (?1049h/l, ?47h/l). Unknown sequences are
 * consumed and ignored so a misbehaving application can never wedge
 * the parser.
 */

enum {
    ANSI_NORMAL = 0,
    ANSI_ESC,   /* got ESC, waiting for '[' or 'O' */
    ANSI_CSI,   /* inside a control sequence, accumulating parameters */
    ANSI_SS3,   /* ESC O ... single-shift (e.g. ESC O H = Home) */
};

static int g_ansi_state = ANSI_NORMAL;
static char g_csi_buf[32];
static int g_csi_len;
static bool g_csi_private;   /* '?' prefix: private (DEC) mode */
static bool g_cursor_visible = true;
static bool g_reverse;       /* SGR 7: swap fg/bg at write time */

/* Alternate screen storage (ESC[?1049h / ESC[?47h). */
static struct fb_cell g_alt_text[FB_MAX_ROWS][FB_MAX_COLS];
static int g_alt_cursor_x, g_alt_cursor_y;
static uint32_t g_alt_fg, g_alt_bg;
static uint8_t g_alt_cur_color;
static bool g_alt_active;

/* The 16 standard ANSI colours (VGA palette, bright variants in the
 * second half). SGR 30-37 / 90-97 select the foreground, 40-47 /
 * 100-107 the background. */
static const uint32_t ansi_colors[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

/* Recompute the palette index for the current (fg,bg,reverse) state.
 * Reverse video stores the swapped pair so later redraws (scroll,
 * cursor blink) reproduce the inversion without per-cell flags. */
static void fb_update_color(void) {
    if (g_reverse) {
        g_cur_color = palette_lookup(g_bg, g_fg);
    } else {
        g_cur_color = palette_lookup(g_fg, g_bg);
    }
}

/* Erase the live cursor cell (before it moves) and repaint the cell
 * at (x,y) with the inverted cursor if it is visible. */
static void fb_repaint_cursor(void) {
    if (g_cursor_visible) {
        fb_draw_cell(g_cursor_y, g_cursor_x, true);
    }
}

/* Parse the accumulated CSI parameters into a small integer array.
 * Missing/empty parameters default to `def` (the first one only -
 * real terminals apply the default to every empty field, but no TUS
 * application relies on that). */
static void ansi_parse_params(int *params, int max, int def) {
    int i = 0, n = 0;
    while (i < g_csi_len && n < max) {
        int v = 0;
        bool any = false;
        while (i < g_csi_len && g_csi_buf[i] >= '0' && g_csi_buf[i] <= '9') {
            v = v * 10 + (g_csi_buf[i] - '0');
            if (v > 9999) {
                v = 9999;
            }
            any = true;
            i++;
        }
        params[n++] = any ? v : def;
        if (i < g_csi_len && g_csi_buf[i] == ';') {
            i++;
        } else {
            break;
        }
    }
    while (n < max) {
        params[n++] = def;
    }
}

/* Blank [row][col0..col1) with the current colour pair. */
static void ansi_erase_cells(int row, int col0, int col1) {
    if (row < 0 || row >= g_rows) {
        return;
    }
    struct fb_cell blank = { ' ', g_cur_color };
    for (int c = col0; c < col1 && c < g_cols; c++) {
        if (c < 0) {
            continue;
        }
        g_text[row][c] = blank;
        fb_paint_cell(row, c, &blank, false);
    }
}

static void ansi_alt_enter(void) {
    if (g_alt_active) {
        return;
    }
    memcpy(g_alt_text, g_text, sizeof(g_alt_text));
    g_alt_cursor_x = g_cursor_x;
    g_alt_cursor_y = g_cursor_y;
    g_alt_fg = g_fg;
    g_alt_bg = g_bg;
    g_alt_cur_color = g_cur_color;
    g_alt_active = true;
    fb_clear();
}

static void ansi_alt_exit(void) {
    if (!g_alt_active) {
        return;
    }
    g_alt_active = false;
    /* Wipe the alternate screen, then put the saved one back. */
    struct fb_cell blank = { ' ', g_cur_color };
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            g_text[r][c] = blank;
        }
    }
    memset(g_pixels, 0, (size_t)(g_pitch_bytes * g_height));
    memcpy(g_text, g_alt_text, sizeof(g_text));
    g_cursor_x = g_alt_cursor_x;
    g_cursor_y = g_alt_cursor_y;
    g_fg = g_alt_fg;
    g_bg = g_alt_bg;
    g_cur_color = g_alt_cur_color;
    fb_redraw_live();
}

/* Run one finished control sequence. `final` is the final byte in
 * 0x40..0x7E; the parameters live in g_csi_buf. */
static void ansi_exec(char final) {
    int params[8];
    ansi_parse_params(params, 8, 1);

    /* Private (DEC) modes: cursor visibility, alternate screen. */
    if (g_csi_private) {
        int m = params[0];
        if (final == 'h') {
            if (m == 25) {
                g_cursor_visible = true;
            } else if (m == 47 || m == 1049) {
                ansi_alt_enter();
            } else if (m == 1048) {
                g_alt_cursor_x = g_cursor_x;
                g_alt_cursor_y = g_cursor_y;
            }
        } else if (final == 'l') {
            if (m == 25) {
                g_cursor_visible = false;
            } else if (m == 47 || m == 1049) {
                ansi_alt_exit();
            } else if (m == 1048) {
                g_cursor_x = g_alt_cursor_x;
                g_cursor_y = g_alt_cursor_y;
            }
        }
        return;
    }

    /* Erase the cursor from its current cell before any move. */
    fb_draw_cell(g_cursor_y, g_cursor_x, false);

    switch (final) {
    case 'H': /* CUP: row;col (1-based), defaults 1;1 */
    case 'f':
        g_cursor_y = params[0] - 1;
        g_cursor_x = params[1] - 1;
        if (g_cursor_y < 0) {
            g_cursor_y = 0;
        }
        if (g_cursor_y >= g_rows) {
            g_cursor_y = g_rows - 1;
        }
        if (g_cursor_x < 0) {
            g_cursor_x = 0;
        }
        if (g_cursor_x >= g_cols) {
            g_cursor_x = g_cols - 1;
        }
        break;
    case 'A': /* cursor up */
        g_cursor_y -= params[0];
        if (g_cursor_y < 0) {
            g_cursor_y = 0;
        }
        break;
    case 'B': /* cursor down */
        g_cursor_y += params[0];
        if (g_cursor_y >= g_rows) {
            g_cursor_y = g_rows - 1;
        }
        break;
    case 'C': /* cursor right */
        g_cursor_x += params[0];
        if (g_cursor_x >= g_cols) {
            g_cursor_x = g_cols - 1;
        }
        break;
    case 'D': /* cursor left */
        g_cursor_x -= params[0];
        if (g_cursor_x < 0) {
            g_cursor_x = 0;
        }
        break;
    case 'G': /* column absolute */
    case '`':
        g_cursor_x = params[0] - 1;
        if (g_cursor_x < 0) {
            g_cursor_x = 0;
        }
        if (g_cursor_x >= g_cols) {
            g_cursor_x = g_cols - 1;
        }
        break;
    case 'd': /* row absolute */
        g_cursor_y = params[0] - 1;
        if (g_cursor_y < 0) {
            g_cursor_y = 0;
        }
        if (g_cursor_y >= g_rows) {
            g_cursor_y = g_rows - 1;
        }
        break;
    case 'J': /* erase display */
        if (params[0] == 2 || params[0] == 3) {
            for (int r = 0; r < g_rows; r++) {
                ansi_erase_cells(r, 0, g_cols);
            }
        } else if (params[0] == 1) {
            for (int r = 0; r <= g_cursor_y; r++) {
                int last = (r == g_cursor_y) ? g_cursor_x + 1 : g_cols;
                ansi_erase_cells(r, 0, last);
            }
        } else { /* 0 or empty: cursor to end of screen */
            ansi_erase_cells(g_cursor_y, g_cursor_x, g_cols);
            for (int r = g_cursor_y + 1; r < g_rows; r++) {
                ansi_erase_cells(r, 0, g_cols);
            }
        }
        break;
    case 'K': /* erase line */
        if (params[0] == 2) {
            ansi_erase_cells(g_cursor_y, 0, g_cols);
        } else if (params[0] == 1) {
            ansi_erase_cells(g_cursor_y, 0, g_cursor_x + 1);
        } else { /* 0 or empty: cursor to end of line */
            ansi_erase_cells(g_cursor_y, g_cursor_x, g_cols);
        }
        break;
    case 'X': /* erase n characters */
        ansi_erase_cells(g_cursor_y, g_cursor_x, g_cursor_x + params[0]);
        break;
    case 'm': /* SGR */
        for (int i = 0; i < 8; i++) {
            int p = params[i];
            if (p == 0) { /* reset */
                g_reverse = false;
                g_fg = COLOR_FG;
                g_bg = COLOR_BG;
            } else if (p == 7) {
                g_reverse = true;
            } else if (p == 27) {
                g_reverse = false;
            } else if (p == 1 || p == 4) {
                /* bold/underline: no font support, ignore */
            } else if (p >= 30 && p <= 37) {
                g_fg = ansi_colors[p - 30];
            } else if (p >= 40 && p <= 47) {
                g_bg = ansi_colors[p - 40];
            } else if (p >= 90 && p <= 97) {
                g_fg = ansi_colors[p - 90 + 8];
            } else if (p >= 100 && p <= 107) {
                g_bg = ansi_colors[p - 100 + 8];
            } else if (p == 39) {
                g_fg = COLOR_FG;
            } else if (p == 49) {
                g_bg = COLOR_BG;
            }
        }
        fb_update_color();
        break;
    default: /* unknown sequence: consume and ignore */
        break;
    }

    fb_repaint_cursor();
}

/* Feed one byte into the escape state machine. Returns true if the
 * byte was consumed as part of a sequence (nothing was drawn). */
static bool ansi_consume(char c) {
    switch (g_ansi_state) {
    case ANSI_NORMAL:
        if (c == 0x1B) {
            g_ansi_state = ANSI_ESC;
            return true;
        }
        return false;
    case ANSI_ESC:
        if (c == '[') {
            g_ansi_state = ANSI_CSI;
            g_csi_len = 0;
            g_csi_private = false;
        } else if (c == 'O') {
            g_ansi_state = ANSI_SS3;
        } else {
            g_ansi_state = ANSI_NORMAL;
        }
        return true;
    case ANSI_SS3:
        /* ESC O H / ESC O F: Home/End from the application cursor
         * keys. Treated as a no-op (kilo only reads these, never
         * writes them). */
        g_ansi_state = ANSI_NORMAL;
        return true;
    case ANSI_CSI:
        if (c >= 0x40 && c <= 0x7E) { /* final byte */
            g_ansi_state = ANSI_NORMAL;
            ansi_exec(c);
            return true;
        }
        if (g_csi_len == 0 && c == '?') {
            g_csi_private = true;
            return true;
        }
        if ((c >= '0' && c <= '9') || c == ';') {
            if (g_csi_len < (int)sizeof(g_csi_buf) - 1) {
                g_csi_buf[g_csi_len++] = c;
            }
            return true;
        }
        /* Intermediate or unexpected byte: give up on this sequence. */
        g_ansi_state = ANSI_NORMAL;
        return true;
    default:
        g_ansi_state = ANSI_NORMAL;
        return true;
    }
}

/* Scrollback history ring. */
static struct fb_cell g_history[FB_HISTORY_ROWS][FB_MAX_COLS];
static int g_hist_head;   /* next slot to write */
static int g_hist_count;  /* number of stored lines (<= FB_HISTORY_ROWS) */
static int g_view_back;   /* lines we have scrolled back (0 = live) */

/* Look up (fg,bg) in the palette, adding it if room remains.
 * Returns the palette index; falls back to index 0 on overflow. */
static uint8_t palette_lookup(uint32_t fg, uint32_t bg) {
    for (int i = 0; i < g_palette_count; i++) {
        if (g_palette_fg[i] == fg && g_palette_bg[i] == bg) {
            return (uint8_t)i;
        }
    }
    if (g_palette_count < FB_PALETTE_SIZE) {
        int i = g_palette_count++;
        g_palette_fg[i] = fg;
        g_palette_bg[i] = bg;
        return (uint8_t)i;
    }
    return 0;
}

/* Paint one cell from an explicit character and color index.
 * Used both by the live path and by scrollback redraws. */
static void fb_paint_cell(int row, int col, const struct fb_cell *cell,
                          bool show_cursor) {
    if (row < 0 || row >= g_rows || col < 0 || col >= g_cols) {
        return;
    }

    uint8_t uc = (uint8_t)cell->c;
    if (uc < FONT_FIRST || uc > FONT_LAST) {
        uc = ' ';
    }

    uint32_t fg = g_palette_fg[cell->color & (FB_PALETTE_SIZE - 1)];
    uint32_t bg = g_palette_bg[cell->color & (FB_PALETTE_SIZE - 1)];
    if (show_cursor && row == g_cursor_y && col == g_cursor_x) {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    const uint8_t *glyph = font8x16[uc - FONT_FIRST];
    uint32_t *pixel = g_pixels
                    + (uint64_t)(g_text_top + (uint32_t)row * FONT_HEIGHT) * g_pitch_words
                    + (uint64_t)col * FONT_WIDTH;

    for (int y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < FONT_WIDTH; x++) {
            pixel[y * g_pitch_words + x] = (bits & (0x80 >> x)) ? fg : bg;
        }
    }
}

/* Redraw one cell from the live text buffer. With show_cursor set,
 * the cursor cell is inverted. */
static void fb_draw_cell(int row, int col, bool show_cursor) {
    fb_paint_cell(row, col, &g_text[row][col], show_cursor);
}

/* Redraw the whole screen from the live text buffer (used when
 * returning from scrollback view). */
static void fb_redraw_live(void) {
    for (int row = 0; row < g_rows; row++) {
        for (int col = 0; col < g_cols; col++) {
            fb_draw_cell(row, col, false);
        }
    }
    if (g_cursor_visible) {
        fb_draw_cell(g_cursor_y, g_cursor_x, true);
    }
}

/* Redraw the whole screen from the scrollback history. g_view_back
 * lines above the live bottom edge are shown; row 0 of the screen
 * maps to history line (g_hist_count - g_view_back - g_rows). */
static void fb_redraw_history(void) {
    static const struct fb_cell blank = { ' ', 0 };
    for (int row = 0; row < g_rows; row++) {
        int hist = g_hist_count - g_view_back - g_rows + row;
        if (hist < 0) {
            for (int col = 0; col < g_cols; col++) {
                fb_paint_cell(row, col, &blank, false);
            }
            continue;
        }
        int slot = (g_hist_head - g_hist_count + hist) % FB_HISTORY_ROWS;
        if (slot < 0) {
            slot += FB_HISTORY_ROWS;
        }
        for (int col = 0; col < g_cols; col++) {
            fb_paint_cell(row, col, &g_history[slot][col], false);
        }
    }
}

/* Record a completed line (the one the cursor just left) into the
 * scrollback history ring. */
static void history_push(void) {
    memcpy(g_history[g_hist_head], g_text[g_cursor_y],
           (size_t)g_cols * sizeof(struct fb_cell));
    g_hist_head = (g_hist_head + 1) % FB_HISTORY_ROWS;
    if (g_hist_count < FB_HISTORY_ROWS) {
        g_hist_count++;
    }
}

void fb_scroll_page(int dir) {
    int max_back = g_hist_count - g_rows;
    if (max_back < 0) {
        max_back = 0;
    }
    g_view_back += dir * g_rows;
    if (g_view_back < 0) {
        g_view_back = 0;
    }
    if (g_view_back > max_back) {
        g_view_back = max_back;
    }
    if (g_view_back == 0) {
        fb_redraw_live();
    } else {
        fb_redraw_history();
    }
}

bool fb_view_scrolled(void) {
    return g_view_back != 0;
}

/* Move every row up by one and clear the bottom row. The pixel
 * shift only touches the text region (below g_text_top), so the boot
 * splash logos above it stay put. */
static void fb_scroll_up(void) {
    for (int row = 1; row < g_rows; row++) {
        memcpy(g_text[row - 1], g_text[row],
               (size_t)g_cols * sizeof(struct fb_cell));
    }
    struct fb_cell blank = { ' ', g_cur_color };
    for (int col = 0; col < g_cols; col++) {
        g_text[g_rows - 1][col] = blank;
    }

    uint8_t *bytes = (uint8_t *)g_pixels + (uint64_t)g_text_top * g_pitch_bytes;
    uint64_t line_bytes = (uint64_t)FONT_HEIGHT * g_pitch_bytes;
    uint64_t total_bytes = (uint64_t)(g_rows - 1) * line_bytes;
    memmove(bytes, bytes + line_bytes, total_bytes);
    memset(bytes + total_bytes, 0, line_bytes);
}

int fb_init(struct limine_framebuffer *fb) {
    if (fb == NULL || fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) {
        return -1;
    }

    g_fb = fb;
    g_pixels = (uint32_t *)fb->address;
    g_pitch_bytes = fb->pitch;
    g_pitch_words = fb->pitch / 4;
    g_width = fb->width;
    g_height = fb->height;

    g_cols = (int)(g_width / FONT_WIDTH);
    g_rows = (int)(g_height / FONT_HEIGHT);
    if (g_cols > FB_MAX_COLS) {
        g_cols = FB_MAX_COLS;
    }
    if (g_rows > FB_MAX_ROWS) {
        g_rows = FB_MAX_ROWS;
    }

    /* Seed the palette with the default color pair. */
    g_palette_count = 0;
    g_cur_color = palette_lookup(COLOR_FG, COLOR_BG);

    fb_clear();
    return 0;
}

void fb_clear(void) {
    struct fb_cell blank = { ' ', g_cur_color };
    for (int row = 0; row < g_rows; row++) {
        for (int col = 0; col < g_cols; col++) {
            g_text[row][col] = blank;
        }
    }
    memset(g_pixels, 0, (size_t)(g_pitch_bytes * g_height));
    g_text_top = 0; /* the splash band only exists during boot */
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_hist_head = 0;
    g_hist_count = 0;
    g_view_back = 0;
}

void fb_putchar(char c) {
    /* Escape sequences never reach the normal drawing path. */
    if (ansi_consume(c)) {
        return;
    }

    /* New output snaps the view back to the live bottom edge. */
    if (g_view_back != 0) {
        g_view_back = 0;
        fb_redraw_live();
    }

    /* Erase the cursor from its current cell first. */
    fb_draw_cell(g_cursor_y, g_cursor_x, false);

    switch (c) {
    case '\n':
        history_push(); /* the line the cursor just left is complete */
        g_cursor_x = 0;
        g_cursor_y++;
        break;
    case '\r':
        g_cursor_x = 0;
        break;
    case '\b':
        if (g_cursor_x > 0) {
            g_cursor_x--;
            g_text[g_cursor_y][g_cursor_x].c = ' ';
            g_text[g_cursor_y][g_cursor_x].color = g_cur_color;
            fb_draw_cell(g_cursor_y, g_cursor_x, false);
        }
        break;
    case '\t':
        do {
            fb_putchar(' ');
        } while (g_cursor_x % 8 != 0);
        return;
    default:
        if ((uint8_t)c >= FONT_FIRST && (uint8_t)c <= FONT_LAST) {
            g_text[g_cursor_y][g_cursor_x].c = c;
            g_text[g_cursor_y][g_cursor_x].color = g_cur_color;
            fb_draw_cell(g_cursor_y, g_cursor_x, false);
            g_cursor_x++;
        }
        break;
    }

    if (g_cursor_x >= g_cols) {
        history_push(); /* the wrapped line is complete */
        g_cursor_x = 0;
        g_cursor_y++;
    }
    if (g_cursor_y >= g_rows) {
        fb_scroll_up();
        g_cursor_y = g_rows - 1;
    }

    /* Draw the cursor at its new position (when visible). */
    if (g_cursor_visible) {
        fb_draw_cell(g_cursor_y, g_cursor_x, true);
    }
}

void fb_set_color(uint32_t fg, uint32_t bg) {
    g_fg = fg;
    g_bg = bg;
    fb_update_color();
}

/* Report the text grid size (columns x rows) for TIOCGWINSZ. */
void fb_get_grid(int *cols, int *rows) {
    if (cols) {
        *cols = g_cols;
    }
    if (rows) {
        *rows = g_rows;
    }
}

void fb_fill(uint32_t color) {
    uint32_t *p = g_pixels;
    uint64_t words = (g_pitch_bytes / 4) * g_height;
    for (uint64_t i = 0; i < words; i++) {
        p[i] = color;
    }
}

/* Start the text grid `pixel_y` pixels below the top of the screen
 * (the boot splash draws its logos above this line). */
void fb_set_text_top(uint32_t pixel_y) {
    g_text_top = pixel_y;
}

/* Draw a scaled RGB image (nearest neighbour) with its top-left
 * corner at (x, y). `scale` is a 16.16 fixed-point factor: an output
 * pixel of size 1x1 samples src[oy*scale>>16][ox*scale>>16]. */
void fb_blit_scaled(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    const uint8_t *rgb, uint32_t scale) {
    if (g_pixels == NULL || rgb == NULL || scale == 0) {
        return;
    }

    uint32_t out_w = (uint32_t)(((uint64_t)w * scale) >> 16);
    uint32_t out_h = (uint32_t)(((uint64_t)h * scale) >> 16);
    if (out_w == 0 || out_h == 0) {
        return;
    }

    for (uint32_t oy = 0; oy < out_h; oy++) {
        uint32_t sy = (uint32_t)(((uint64_t)oy * h) / out_h);
        if (sy >= h) {
            sy = h - 1;
        }
        uint32_t py = y + oy;
        if (py >= g_height) {
            break;
        }
        const uint8_t *src_row = rgb + (uint64_t)sy * w * 3;
        uint32_t *dst = g_pixels + (uint64_t)py * g_pitch_words + x;

        for (uint32_t ox = 0; ox < out_w; ox++) {
            uint32_t sx = (uint32_t)(((uint64_t)ox * w) / out_w);
            if (sx >= w) {
                sx = w - 1;
            }
            uint32_t px = x + ox;
            if (px >= g_width) {
                break;
            }
            const uint8_t *s = src_row + (uint64_t)sx * 3;
            dst[ox] = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
        }
    }
}

void fb_get_info(uint32_t *width, uint32_t *height, uint32_t *bpp,
                 uint64_t *pitch, void **address) {
    if (width) {
        *width = g_width;
    }
    if (height) {
        *height = g_height;
    }
    if (bpp) {
        *bpp = g_fb ? g_fb->bpp : 0;
    }
    if (pitch) {
        *pitch = g_pitch_bytes;
    }
    if (address) {
        *address = (void *)g_pixels;
    }
}
