/*
 * pmm.c - Physical Memory Manager implementation
 *
 * Only LIMINE_MEMMAP_USABLE regions are handed out. Everything else
 * (kernel image, framebuffer, ACPI tables, bootloader internals) is
 * marked non-usable by Limine and never touched. BOOTLOADER_RECLAIMABLE
 * regions are intentionally left alone for now: the kernel still runs
 * on Limine's page tables and stack, which live there.
 *
 * The bitmap is a static array sized for up to 4 GiB of RAM
 * (1 Mi frames -> 128 KiB of bitmap). Bumping this limit later only
 * means making the bitmap dynamic.
 */

#include "pmm.h"

#include <stdbool.h>

#include "../core/klib.h"

#define FRAME_SIZE   4096
#define FRAME_SHIFT  12

#define MAX_FRAMES   (1024 * 1024) /* 4 GiB of RAM */

#define BITMAP_BYTES (MAX_FRAMES / 8)

static uint8_t g_bitmap[BITMAP_BYTES];
static uint64_t g_total_frames;
static uint64_t g_allocated_frames;
static uint64_t g_hhdm_offset;
static uint64_t g_last_hint; /* next search position (first-fit cache) */

static bool frame_is_free(uint64_t frame) {
    return !(g_bitmap[frame / 8] & (1u << (frame % 8)));
}

static void frame_set_used(uint64_t frame) {
    g_bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static void frame_set_free(uint64_t frame) {
    g_bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

void pmm_init(const struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    g_hhdm_offset = hhdm_offset;

    /* Everything is used until proven usable. */
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));
    g_total_frames = 0;
    g_allocated_frames = 0;

    if (memmap == NULL) {
        return;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        const struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uint64_t start = (entry->base + FRAME_SIZE - 1) >> FRAME_SHIFT;
        uint64_t end = (entry->base + entry->length) >> FRAME_SHIFT;
        if (end > MAX_FRAMES) {
            end = MAX_FRAMES;
        }

        for (uint64_t frame = start; frame < end; frame++) {
            frame_set_free(frame);
            g_total_frames++;
        }
    }

    g_last_hint = 0;
}

uint64_t pmm_alloc_frame(void) {
    /* First-fit scan starting from the last allocation position. */
    for (uint64_t i = g_last_hint; i < g_total_frames; i++) {
        if (frame_is_free(i)) {
            frame_set_used(i);
            g_allocated_frames++;
            g_last_hint = i + 1;
            return i << FRAME_SHIFT;
        }
    }
    /* Wrap around for frames before the hint. */
    for (uint64_t i = 0; i < g_last_hint && i < g_total_frames; i++) {
        if (frame_is_free(i)) {
            frame_set_used(i);
            g_allocated_frames++;
            g_last_hint = i + 1;
            return i << FRAME_SHIFT;
        }
    }
    return 0; /* out of memory */
}

uint64_t pmm_alloc_frames(size_t count) {
    if (count == 0) {
        return 0;
    }
    for (uint64_t i = 0; i + count <= g_total_frames; i++) {
        bool ok = true;
        for (size_t j = 0; j < count; j++) {
            if (!frame_is_free(i + j)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (size_t j = 0; j < count; j++) {
                frame_set_used(i + j);
            }
            g_allocated_frames += count;
            g_last_hint = i + count;
            return i << FRAME_SHIFT;
        }
    }
    return 0;
}

void pmm_free_frame(uint64_t phys) {
    uint64_t frame = phys >> FRAME_SHIFT;
    if (frame >= g_total_frames) {
        return;
    }
    if (frame_is_free(frame)) {
        return; /* double free: ignore */
    }
    frame_set_free(frame);
    g_allocated_frames--;
}

void pmm_free_frames(uint64_t phys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_free_frame(phys + i * FRAME_SIZE);
    }
}

uint64_t pmm_phys_to_virt(uint64_t phys) {
    return phys + g_hhdm_offset;
}

void pmm_get_stats(uint64_t *total_frames, uint64_t *free_frames) {
    if (total_frames) {
        *total_frames = g_total_frames;
    }
    if (free_frames) {
        *free_frames = g_total_frames - g_allocated_frames;
    }
}
