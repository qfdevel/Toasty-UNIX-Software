/*
 * ppm.h - Netpbm PPM image decoder
 *
 * Decodes PPM images straight from raw file bytes - no build-time
 * conversion, no embedded arrays. Both ASCII (P3) and binary (P6)
 * variants are supported, with 8-bit channels and any maxval.
 *
 * The decoded image is stored as tightly packed RGB triplets
 * (width*height*3 bytes, row-major), ready to be blitted or scaled
 * onto the framebuffer.
 */

#ifndef TUS_DRIVERS_PPM_H
#define TUS_DRIVERS_PPM_H

#include <stddef.h>
#include <stdint.h>

struct ppm_image {
    uint32_t width;
    uint32_t height;
    uint8_t *rgb; /* width * height * 3 bytes; kmalloc'd, caller frees */
};

/* Decode `size` bytes of PPM data into *img. On success img->rgb is
 * allocated with kmalloc (kfree it when done) and 0 is returned.
 * Returns -1 on any parse error or out-of-memory condition. */
int ppm_decode(const void *data, size_t size, struct ppm_image *img);

#endif /* TUS_DRIVERS_PPM_H */
