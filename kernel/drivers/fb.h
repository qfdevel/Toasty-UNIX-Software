/*
 * fb.h - framebuffer text console
 *
 * Draws an 8x16 text grid over the framebuffer that Limine hands us.
 * This driver owns the raw pixel surface; a future /dev/fb0 device
 * will expose exactly this memory to userspace.
 */

#ifndef TUS_DRIVERS_FB_H
#define TUS_DRIVERS_FB_H

#include <stdbool.h>
#include <stdint.h>

#include <limine.h>

/* Initialize the console for the given framebuffer.
 * Returns 0 on success, -1 if the framebuffer is unusable. */
int fb_init(struct limine_framebuffer *fb);

/* Write one character at the cursor (handles \n \r \b \t). */
void fb_putchar(char c);

/* Clear the screen and reset the cursor to the top-left corner. */
void fb_clear(void);

/* Scroll the visible window one page. dir > 0 looks back into the
 * scrollback history (PageUp), dir < 0 returns towards the live
 * output (PageDown). Any new output snaps back to live mode. */
void fb_scroll_page(int dir);

/* True while the window is scrolled back into history. */
bool fb_view_scrolled(void);

/* Set the colors used for subsequently drawn text. */
void fb_set_color(uint32_t fg, uint32_t bg);

/* Fill the whole framebuffer with one color (pixels only; the text
 * buffer is untouched so on-screen text survives a redraw). */
void fb_fill(uint32_t color);

/* Copy the framebuffer description; any pointer may be NULL. */
void fb_get_info(uint32_t *width, uint32_t *height, uint32_t *bpp,
                 uint64_t *pitch, void **address);

/* Report the text grid size (columns x rows) - what TIOCGWINSZ
 * returns to user programs. */
void fb_get_grid(int *cols, int *rows);

#endif /* TUS_DRIVERS_FB_H */
