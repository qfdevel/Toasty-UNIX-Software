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

static char g_text[FB_MAX_ROWS][FB_MAX_COLS];

/* Redraw one cell. With show_cursor set, the cursor cell is inverted. */
static void fb_draw_cell(int row, int col, bool show_cursor) {
    if (row < 0 || row >= g_rows || col < 0 || col >= g_cols) {
        return;
    }

    uint8_t uc = (uint8_t)g_text[row][col];
    if (uc < FONT_FIRST || uc > FONT_LAST) {
        uc = ' ';
    }

    uint32_t fg = g_fg;
    uint32_t bg = g_bg;
    if (show_cursor && row == g_cursor_y && col == g_cursor_x) {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    const uint8_t *glyph = font8x16[uc - FONT_FIRST];
    uint32_t *cell = g_pixels + (uint64_t)(row * FONT_HEIGHT) * g_pitch_words
                   + (uint64_t)col * FONT_WIDTH;

    for (int y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < FONT_WIDTH; x++) {
            cell[y * g_pitch_words + x] = (bits & (0x80 >> x)) ? fg : bg;
        }
    }
}

/* Move every row up by one and clear the bottom row. */
static void fb_scroll_up(void) {
    for (int row = 1; row < g_rows; row++) {
        memcpy(g_text[row - 1], g_text[row], (size_t)g_cols);
    }
    memset(g_text[g_rows - 1], ' ', (size_t)g_cols);

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

    fb_clear();
    return 0;
}

void fb_clear(void) {
    for (int row = 0; row < g_rows; row++) {
        memset(g_text[row], ' ', (size_t)g_cols);
    }
    memset(g_pixels, 0, (size_t)(g_pitch_bytes * g_height));
    g_cursor_x = 0;
    g_cursor_y = 0;
}

void fb_putchar(char c) {
    /* Erase the cursor from its current cell first. */
    fb_draw_cell(g_cursor_y, g_cursor_x, false);

    switch (c) {
    case '\n':
        g_cursor_x = 0;
        g_cursor_y++;
        break;
    case '\r':
        g_cursor_x = 0;
        break;
    case '\b':
        if (g_cursor_x > 0) {
            g_cursor_x--;
            g_text[g_cursor_y][g_cursor_x] = ' ';
            fb_draw_cell(g_cursor_y, g_cursor_x, false);
        }
        break;
    case '\t':
        do {
            fb_putchar(' ');
        } while (g_cursor_x % 8 != 0);
        break;
    default:
        if ((uint8_t)c >= FONT_FIRST && (uint8_t)c <= FONT_LAST) {
            g_text[g_cursor_y][g_cursor_x] = c;
            fb_draw_cell(g_cursor_y, g_cursor_x, false);
            g_cursor_x++;
        }
        break;
    }

    if (g_cursor_x >= g_cols) {
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
