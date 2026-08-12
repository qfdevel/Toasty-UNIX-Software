/*
 * pmm.h - Physical Memory Manager (page frame allocator)
 *
 * Allocates 4 KiB physical frames from the usable memory regions that
 * the Limine boot protocol reports. One bitmap bit per frame: set =
 * allocated, clear = free.
 */

#ifndef TUS_MM_PMM_H
#define TUS_MM_PMM_H

#include <stddef.h>
#include <stdint.h>

#include <limine.h>

/* Initialize the allocator from the Limine memory map.
 * `hhdm_offset` is the higher-half direct map offset (phys -> virt). */
void pmm_init(const struct limine_memmap_response *memmap, uint64_t hhdm_offset);

/* Allocate one frame. Returns the PHYSICAL address, or 0 on failure. */
uint64_t pmm_alloc_frame(void);

/* Allocate `count` contiguous frames. Returns base phys or 0 on failure. */
uint64_t pmm_alloc_frames(size_t count);

/* Release one (or `count` contiguous) frame(s). */
void pmm_free_frame(uint64_t phys);
void pmm_free_frames(uint64_t phys, size_t count);

/* Translate a physical address to its higher-half virtual address. */
uint64_t pmm_phys_to_virt(uint64_t phys);

/* Statistics for `sysinfo`. */
void pmm_get_stats(uint64_t *total_frames, uint64_t *free_frames);

#endif /* TUS_MM_PMM_H */
