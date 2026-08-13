/*
 * splash.h - boot splash: one toast per CPU + boot logs
 *
 * Mirrors what Linux does with the Tux logos: at boot the kernel
 * counts the CPUs (Limine MP feature) and draws that many toast
 * logos across the top of the framebuffer - one per core. The boot
 * log then scrolls below the logo band. When the shell starts, the
 * screen is cleared and the console takes over normally.
 *
 * The logo is NOT compiled into the kernel: splash_show() reads
 * /logo.ppm from the root filesystem and decodes it with the PPM
 * driver, so the image can be swapped by editing rootfs.img.
 */

#ifndef TUS_BOOT_SPLASH_H
#define TUS_BOOT_SPLASH_H

#include <stdint.h>

/* Draw `cpu_count` toasts (scaled to fit) and move the text console
 * below them. Safe to call before interrupts and the scheduler are
 * up; returns 0 on success, -1 if there is no framebuffer or the
 * logo cannot be loaded. */
int splash_show(uint64_t cpu_count);

#endif /* TUS_BOOT_SPLASH_H */
