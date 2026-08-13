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

/* Physical address of the boot (root) address space - the one the
 * kernel shell runs in. All per-task spaces clone its kernel half. */
uint64_t vmm_root_cr3(void);

/* Allocate a fresh address space: a new PML4 whose kernel half
 * (indices 256..511) is copied from the root space, so every task
 * sees the same kernel mappings. The user half starts empty.
 * Returns the new CR3, or 0 on failure. */
uint64_t vmm_space_clone(void);

/* Make `cr3` the active address space (g_cr3 + CR3 reload, which
 * also flushes the TLB). The caller must be in a context where the
 * current kernel stack is mapped in the new space (always true:
 * kernel stacks live in the shared kernel heap). */
void vmm_space_switch(uint64_t cr3);

/* Map a single 4 KiB page into an explicit address space.
 * `virt` must be page aligned. Returns 0 on success, -1 on failure. */
int vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys,
                    uint64_t flags);

/* Ensure the intermediate page tables for [virt, virt+bytes) exist
 * in the root space (no frames mapped). Call before any task can
 * exist, so later runtime mappings only touch shared leaf tables
 * and every address space always sees them. */
void vmm_reserve_tables(uint64_t virt, size_t bytes);

/* Map a single 4 KiB page in the CURRENT address space. Kernel-half
 * addresses (>= 0xffff800000000000) are mapped in the root space
 * instead: the kernel half is shared by reference, and mapping there
 * keeps every task's view consistent. Returns 0 or -1. */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Map `bytes` (page aligned) of physical memory at `virt`. */
int vmm_map_region(uint64_t virt, uint64_t phys, size_t bytes, uint64_t flags);

/* Unmap a single page (PTE cleared, TLB flushed). */
void vmm_unmap_page(uint64_t virt);

/* Unmap a single page in an explicit address space (PTE cleared,
 * TLB flushed). The target space does not need to be loaded. */
void vmm_unmap_page_in(uint64_t cr3, uint64_t virt);

/* Translate a virtual address in the current space; 0 if unmapped. */
uint64_t vmm_translate(uint64_t virt);

/* Translate a virtual address in an explicit space; 0 if unmapped. */
uint64_t vmm_translate_in(uint64_t cr3, uint64_t virt);

/* Return the raw page-table entry for a virtual address (0 if any
 * level is unmapped). Debug helper. */
uint64_t vmm_pte(uint64_t virt);

/* Return bitmask of NX bits set in the upper levels (PML4/PDPT/PD)
 * for a virtual address. Debug helper: 0 means executable. */
uint64_t vmm_level_nx(uint64_t virt);

/* Return the raw page-table entry at `level` (1=PD, 2=PDPT, 3=PML4).
 * Debug helper to inspect Limine's large-page mappings. */
uint64_t vmm_level_entry(uint64_t virt, int level);

#endif /* TUS_MM_VMM_H */
