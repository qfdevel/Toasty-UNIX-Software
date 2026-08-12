/*
 * vmm.c - Virtual Memory Manager implementation
 *
 * Page table walk for 4-level paging:
 *
 *     CR3 -> PML4[ idx39:30 ] -> PDPT[ idx29:21 ] -> PD[ idx20:12 ]
 *          -> PT[ idx11:3 ] -> 4 KiB page
 *
 * Missing intermediate tables are allocated from the PMM and zeroed.
 * The upper 12 bits of every non-leaf entry carry the physical address
 * of the next level; the lower 12 bits are flags.
 */

#include "vmm.h"

#include <stdbool.h>

#include "pmm.h"
#include "../core/klib.h"

#define PAGE_SIZE   4096
#define PAGE_SHIFT  12

#define PTE_ADDR_MASK 0x000ffffffffff000ull
#define PTE_FLAG_MASK 0xfff

static uint64_t g_cr3; /* physical address of the PML4 */

static uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Flush one virtual address from the TLB. */
static void invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Fetch the physical address of a page table level, or allocate one. */
static uint64_t table_fetch(uint64_t *entry, bool allocate) {
    if (*entry & VMM_PRESENT) {
        return *entry & PTE_ADDR_MASK;
    }
    if (!allocate) {
        return 0;
    }
    uint64_t phys = pmm_alloc_frame();
    if (phys == 0) {
        return 0;
    }
    memset((void *)pmm_phys_to_virt(phys), 0, PAGE_SIZE);
    *entry = phys | VMM_PRESENT | VMM_WRITE;
    return phys;
}

void vmm_init(void) {
    g_cr3 = read_cr3();
}

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if ((virt & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1))) {
        return -1;
    }

    uint64_t *pml4 = (uint64_t *)pmm_phys_to_virt(g_cr3);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t pdpt_phys = table_fetch(&pml4[pml4_idx], true);
    if (pdpt_phys == 0) {
        return -1;
    }
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pdpt_phys);

    uint64_t pd_phys = table_fetch(&pdpt[pdpt_idx], true);
    if (pd_phys == 0) {
        return -1;
    }
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pd_phys);

    uint64_t pt_phys = table_fetch(&pd[pd_idx], true);
    if (pt_phys == 0) {
        return -1;
    }
    uint64_t *pt = (uint64_t *)pmm_phys_to_virt(pt_phys);

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | VMM_PRESENT;
    invlpg(virt);
    return 0;
}

int vmm_map_region(uint64_t virt, uint64_t phys, size_t bytes, uint64_t flags) {
    for (size_t off = 0; off < bytes; off += PAGE_SIZE) {
        if (vmm_map_page(virt + off, phys + off, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)pmm_phys_to_virt(g_cr3);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & VMM_PRESENT)) {
        return;
    }
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        return;
    }
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & VMM_PRESENT)) {
        return;
    }
    uint64_t *pt = (uint64_t *)pmm_phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);

    pt[pt_idx] = 0;
    invlpg(virt);
}

uint64_t vmm_translate(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)pmm_phys_to_virt(g_cr3);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pt = (uint64_t *)pmm_phys_to_virt(pd[pd_idx] & PTE_ADDR_MASK);
    if (!(pt[pt_idx] & VMM_PRESENT)) {
        return 0;
    }
    return (pt[pt_idx] & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}
