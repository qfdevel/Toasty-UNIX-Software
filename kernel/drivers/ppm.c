/*
 * ppm.c - Netpbm PPM image decoder
 *
 * A small, self-contained parser for the two common PPM flavours:
 *   P3 - ASCII:  "P3\n<w> <h>\n<maxval>\n" then w*h triplets of
 *                decimal numbers separated by whitespace.
 *   P6 - binary: "P6\n<w> <h>\n<maxval>\n" then w*h raw RGB bytes.
 *
 * Comments ('#' to end of line) are honoured in the header. Channel
 * values are scaled from the file's maxval to 8 bits. The result is
 * packed RGB triplets; nothing is drawn here - rendering is up to the
 * caller (see kernel/boot/splash.c).
 */

#include "ppm.h"

#include <stdbool.h>

#include "../core/klib.h"
#include "../mm/kmalloc.h"

/* Token reader over the ASCII header. Returns false at EOF. */
static bool ppm_next_token(const char **p, const char *end,
                           char *out, size_t out_size) {
    while (*p < end) {
        char c = **p;
        if (c == '#') { /* comment until end of line */
            while (*p < end && **p != '\n') {
                (*p)++;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            (*p)++;
            continue;
        }
        break;
    }
    if (*p >= end) {
        return false;
    }

    size_t n = 0;
    while (*p < end && n + 1 < out_size) {
        char c = **p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '#') {
            break;
        }
        out[n++] = c;
        (*p)++;
    }
    out[n] = '\0';
    return n > 0;
}

static bool ppm_parse_u32(const char *tok, uint32_t *out) {
    if (*tok == '\0') {
        return false;
    }
    uint32_t v = 0;
    for (; *tok != '\0'; tok++) {
        if (*tok < '0' || *tok > '9') {
            return false;
        }
        v = v * 10 + (uint32_t)(*tok - '0');
        if (v > 0xFFFF) {
            return false; /* absurd dimensions/values */
        }
    }
    *out = v;
    return true;
}

int ppm_decode(const void *data, size_t size, struct ppm_image *img) {
    if (data == NULL || img == NULL || size < 4) {
        return -1;
    }

    const char *p = (const char *)data;
    const char *end = p + size;
    char tok[16];

    /* Magic number. */
    if (!ppm_next_token(&p, end, tok, sizeof(tok))) {
        return -1;
    }
    bool binary = false;
    if (strcmp(tok, "P3") == 0) {
        binary = false;
    } else if (strcmp(tok, "P6") == 0) {
        binary = true;
    } else {
        return -1; /* unsupported magic */
    }

    uint32_t width, height, maxval;
    if (!ppm_next_token(&p, end, tok, sizeof(tok)) ||
        !ppm_parse_u32(tok, &width) || width == 0 || width > 4096) {
        return -1;
    }
    if (!ppm_next_token(&p, end, tok, sizeof(tok)) ||
        !ppm_parse_u32(tok, &height) || height == 0 || height > 4096) {
        return -1;
    }
    if (!ppm_next_token(&p, end, tok, sizeof(tok)) ||
        !ppm_parse_u32(tok, &maxval) || maxval == 0 || maxval > 65535) {
        return -1;
    }

    size_t npixels = (size_t)width * height;
    uint8_t *rgb = kmalloc(npixels * 3);
    if (rgb == NULL) {
        return -1;
    }

    if (binary) {
        /* Skip whitespace right after the maxval token (P6 allows a
         * single whitespace character before the raster). */
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            p++;
        }
        if ((size_t)(end - p) < npixels * 3) {
            kfree(rgb);
            return -1;
        }
        if (maxval == 255) {
            memcpy(rgb, p, npixels * 3);
        } else {
            for (size_t i = 0; i < npixels * 3; i++) {
                rgb[i] = (uint8_t)(((uint32_t)p[i] * 255) / maxval);
            }
        }
    } else {
        /* ASCII raster: w*h*3 decimal numbers. */
        uint8_t *out = rgb;
        for (size_t i = 0; i < npixels * 3; i++) {
            if (!ppm_next_token(&p, end, tok, sizeof(tok))) {
                kfree(rgb);
                return -1;
            }
            uint32_t v;
            if (!ppm_parse_u32(tok, &v)) {
                kfree(rgb);
                return -1;
            }
            *out++ = (uint8_t)((v * 255) / maxval);
        }
    }

    img->width = width;
    img->height = height;
    img->rgb = rgb;
    return 0;
}
