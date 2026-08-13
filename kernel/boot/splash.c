/*
 * splash.c - boot splash: one toast per CPU + boot logs
 *
 * Reads /logo.ppm from the root filesystem, decodes it, scales it to
 * fit the screen and draws one copy per detected CPU in a row across
 * the top. The text console is then moved below the logo band, so the
 * boot log (printed by main.c) scrolls under the toasts. When tsh
 * starts it calls console_clear(), which wipes the splash and resets
 * the console to a full-screen shell.
 *
 * Layout (all in pixels):
 *     margin  : 24 above the logos
 *     logos   : cpu_count scaled toasts, 20px gaps, centered
 *     text    : starts 16px below the logo band
 */

#include "splash.h"

#include "../core/console.h"
#include "../core/klib.h"
#include "../drivers/fb.h"
#include "../drivers/ppm.h"
#include "../mm/kmalloc.h"
#include "../vfs/vfs.h"

#define SPLASH_MARGIN_TOP 24
#define SPLASH_GAP        20
#define SPLASH_TEXT_GAP   16

/* Fit scale for the toast row: at most 80% of the width and 45% of
 * the height, never upscaled past 1:1. Returns 16.16 fixed point. */
static uint32_t splash_scale(uint64_t count, uint32_t w, uint32_t h,
                             uint32_t img_w, uint32_t img_h) {
    uint64_t limit_w = (uint64_t)w * 80 / 100;
    uint64_t limit_h = (uint64_t)h * 45 / 100;

    uint64_t scale = (uint64_t)1 << 16; /* 1:1 */

    uint64_t row = (uint64_t)count * img_w + (uint64_t)(count - 1) * SPLASH_GAP;
    if (row > 0 && row > limit_w) {
        uint64_t s = (limit_w << 16) / row;
        if (s < scale) {
            scale = s;
        }
    }
    if (img_h > 0 && img_h > limit_h) {
        uint64_t s = (limit_h << 16) / img_h;
        if (s < scale) {
            scale = s;
        }
    }
    return (uint32_t)scale;
}

int splash_show(uint64_t cpu_count) {
    if (!console_has_framebuffer()) {
        return -1;
    }
    if (cpu_count == 0) {
        cpu_count = 1;
    }

    /* Read the logo out of the root filesystem. */
    long fd = vfs_open("/logo.ppm", O_RDONLY);
    if (fd < 0) {
        console_write("splash: /logo.ppm not found, skipping logos\n");
        return -1;
    }
    struct vfs_node *node = vfs_lookup("/logo.ppm");
    if (node == NULL || node->size == 0) {
        vfs_close(fd);
        return -1;
    }
    uint8_t *file = kmalloc(node->size);
    if (file == NULL) {
        vfs_close(fd);
        return -1;
    }
    long got = vfs_read(fd, file, node->size);
    vfs_close(fd);
    if (got != (long)node->size) {
        kfree(file);
        return -1;
    }

    struct ppm_image img;
    if (ppm_decode(file, node->size, &img) != 0) {
        kfree(file);
        console_write("splash: /logo.ppm is not a valid PPM\n");
        return -1;
    }
    kfree(file); /* the decoded image is what we need now */

    uint32_t width, height;
    fb_get_info(&width, &height, NULL, NULL, NULL);

    /* Black screen, then the toast row, centered. */
    fb_fill(0x00000000);
    uint32_t scale = splash_scale(cpu_count, width, height,
                                  img.width, img.height);
    uint32_t toast_w = (uint32_t)(((uint64_t)img.width * scale) >> 16);
    uint32_t toast_h = (uint32_t)(((uint64_t)img.height * scale) >> 16);

    uint64_t row_w = (uint64_t)toast_w * cpu_count
                   + (uint64_t)(cpu_count - 1) * SPLASH_GAP;
    uint32_t x = (row_w < width) ? (uint32_t)((width - row_w) / 2) : 0;

    for (uint64_t i = 0; i < cpu_count; i++) {
        fb_blit_scaled(x + (uint32_t)i * (toast_w + SPLASH_GAP),
                       SPLASH_MARGIN_TOP, img.width, img.height,
                       img.rgb, scale);
    }
    kfree(img.rgb);

    /* The boot log scrolls below the logos. */
    console_set_text_top(SPLASH_MARGIN_TOP + toast_h + SPLASH_TEXT_GAP);
    return 0;
}
