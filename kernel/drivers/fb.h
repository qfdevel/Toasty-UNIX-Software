/*
 * fb.h - framebuffer text console
 *
 * Draws an 8x16 text grid over the framebuffer that Limine hands us.
 * This driver owns the raw pixel surface; a future /dev/fb0 device
 * will expose exactly this memory to userspace.
 */

#ifndef TUS_DRIVERS_FB_H
#define TUS_DRIVERS_FB_H

#include <stdint.h>

#include <limine.h>

/* Initialize the console for the given framebuffer.
 * Returns 0 on success, -1 if the framebuffer is unusable. */
int fb_init(struct limine_framebuffer *fb);

/* Write one character at the cursor (handles \n \r \b \t). */
void fb_putchar(char c);

/* Clear the screen and reset the cursor to the top-left corner. */
void fb_clear(void);

/* Set the colors used for subsequently drawn text. */
void fb_set_color(uint32_t fg, uint32_t bg);

/* Copy the framebuffer description; any pointer may be NULL. */
void fb_get_info(uint32_t *width, uint32_t *height, uint32_t *bpp,
                 uint64_t *pitch, void **address);

#endif /* TUS_DRIVERS_FB_H */
