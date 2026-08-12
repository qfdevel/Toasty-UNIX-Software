/*
 * vmm.h - Virtual Memory Manager (x86-64, 4-level paging)
 *
 * The kernel boots on the page tables that Limine set up (kernel image
 * mapped in the higher half, HHDM covering all physical memory). This
 * module owns those tables: it can walk them and add new mappings on
 * demand. The kernel heap lives in a dedicated higher-half region that
 * is mapped frame by frame as the allocator grows.
 */

#ifndef TUS_MM_VMM_H
#define TUS_MM_VMM_H

#include <stddef.h>
#include <stdint.h>

/* Page table entry flags. */
#define VMM_PRESENT (1ull << 0)
#define VMM_WRITE   (1ull << 1)
#define VMM_USER    (1ull << 2)

/* Virtual address of the kernel heap arena (reserved address space). */
#define VMM_KHEAP_BASE 0xffffffff81000000ull
#define VMM_KHEAP_SIZE (64ull * 1024 * 1024)

/* Initialize: record the active page tables (CR3). */
void vmm_init(void);

/* Map a single 4 KiB page. `virt` must be page aligned.
 * Returns 0 on success, -1 on failure. */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Map `bytes` (page aligned) of physical memory at `virt`. */
int vmm_map_region(uint64_t virt, uint64_t phys, size_t bytes, uint64_t flags);

/* Unmap a single page (PTE cleared, TLB flushed). */
void vmm_unmap_page(uint64_t virt);

/* Translate a virtual address; returns 0 if unmapped. */
uint64_t vmm_translate(uint64_t virt);

#endif /* TUS_MM_VMM_H */
