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
    uint32_t *pixel = g_pixels + (uint64_t)(row * FONT_HEIGHT) * g_pitch_words
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
    fb_draw_cell(g_cursor_y, g_cursor_x, true);
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

/* Move every row up by one and clear the bottom row. */
static void fb_scroll_up(void) {
    for (int row = 1; row < g_rows; row++) {
        memcpy(g_text[row - 1], g_text[row],
               (size_t)g_cols * sizeof(struct fb_cell));
    }
    struct fb_cell blank = { ' ', g_cur_color };
    for (int col = 0; col < g_cols; col++) {
        g_text[g_rows - 1][col] = blank;
    }

    uint8_t *bytes = (uint8_t *)g_pixels;
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
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_hist_head = 0;
    g_hist_count = 0;
    g_view_back = 0;
}

void fb_putchar(char c) {
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

    /* Draw the cursor at its new position. */
    fb_draw_cell(g_cursor_y, g_cursor_x, true);
}

void fb_set_color(uint32_t fg, uint32_t bg) {
    g_fg = fg;
    g_bg = bg;
    g_cur_color = palette_lookup(fg, bg);
}

void fb_fill(uint32_t color) {
    uint32_t *p = g_pixels;
    uint64_t words = (g_pitch_bytes / 4) * g_height;
    for (uint64_t i = 0; i < words; i++) {
        p[i] = color;
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
