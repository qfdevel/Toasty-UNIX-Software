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

/* Fetch the physical address of a page table level, or allocate one.
 * New tables are marked USER as well: on x86-64 the U/S bit of every
 * level participates in the access check, so a supervisor-only upper
 * entry blocks ring-3 access even when the leaf PTE has U set. */
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
    *entry = phys | VMM_PRESENT | VMM_WRITE | VMM_USER;
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

    /* Limine may have mapped this region with large pages (PS bit) or
     * NX set. If an upper-level entry is a large page, split it by
     * replacing it with a freshly allocated table that maps the same
     * frames at 4 KiB granularity. The inherited flags must have NX
     * (bit 63) cleared so the resulting pages stay executable. */
    const uint64_t split_flags_mask = 0xFFF & ~(1ull << 7) & ~(1ull << 63);
    if (pml4[pml4_idx] & (1ull << 7)) {
        /* 1 GiB page in PML4: repoint to a new PDPT. */
        uint64_t base = pml4[pml4_idx] & PTE_ADDR_MASK;
        uint64_t flags_l = pml4[pml4_idx] & split_flags_mask;
        uint64_t new_phys = pmm_alloc_frame();
        if (new_phys == 0) {
            return -1;
        }
        uint64_t *new_pdpt = (uint64_t *)pmm_phys_to_virt(new_phys);
        memset(new_pdpt, 0, PAGE_SIZE);
        for (int i = 0; i < 512; i++) {
            new_pdpt[i] = (base + (uint64_t)i * 0x40000000ull) | flags_l;
        }
        pml4[pml4_idx] = new_phys | (pml4[pml4_idx] & 0xFFF);
    }

    uint64_t pdpt_phys = table_fetch(&pml4[pml4_idx], true);
    if (pdpt_phys == 0) {
        return -1;
    }
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pdpt_phys);

    if (pdpt[pdpt_idx] & (1ull << 7)) {
        /* 1 GiB page in PDPT: split into a new PD of 2 MiB pages. */
        uint64_t base = pdpt[pdpt_idx] & PTE_ADDR_MASK;
        uint64_t flags_l = (pdpt[pdpt_idx] & split_flags_mask) | (1ull << 7);
        uint64_t new_phys = pmm_alloc_frame();
        if (new_phys == 0) {
            return -1;
        }
        uint64_t *new_pd = (uint64_t *)pmm_phys_to_virt(new_phys);
        memset(new_pd, 0, PAGE_SIZE);
        for (int i = 0; i < 512; i++) {
            new_pd[i] = (base + (uint64_t)i * 0x200000ull) | flags_l;
        }
        pdpt[pdpt_idx] = new_phys | (pdpt[pdpt_idx] & 0xFFF);
    }

    uint64_t pd_phys = table_fetch(&pdpt[pdpt_idx], true);
    if (pd_phys == 0) {
        return -1;
    }
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pd_phys);

    if (pd[pd_idx] & (1ull << 7)) {
        /* 2 MiB page in PD: split into a new PT of 4 KiB pages. */
        uint64_t base = pd[pd_idx] & PTE_ADDR_MASK;
        uint64_t flags_l = pd[pd_idx] & split_flags_mask;
        uint64_t new_phys = pmm_alloc_frame();
        if (new_phys == 0) {
            return -1;
        }
        uint64_t *new_pt = (uint64_t *)pmm_phys_to_virt(new_phys);
        memset(new_pt, 0, PAGE_SIZE);
        for (int i = 0; i < 512; i++) {
            new_pt[i] = (base + (uint64_t)i * 0x1000ull) | flags_l;
        }
        pd[pd_idx] = new_phys | (pd[pd_idx] & 0xFFF);
    }

    uint64_t pt_phys = table_fetch(&pd[pd_idx], true);
    if (pt_phys == 0) {
        return -1;
    }
    uint64_t *pt = (uint64_t *)pmm_phys_to_virt(pt_phys);

    /* Limine may have marked upper-level entries NX; clear bit 63 on
     * every level we touch so the mapped page stays executable when
     * the caller did not ask for NX. If the caller wants a USER page,
     * every upper level must carry U/S too: on x86-64 the U/S bit of
     * each level participates in the access check. */
    pml4[pml4_idx] &= ~(1ull << 63);
    pdpt[pdpt_idx] &= ~(1ull << 63);
    pd[pd_idx]     &= ~(1ull << 63);
    if (flags & VMM_USER) {
        pml4[pml4_idx] |= VMM_USER;
        pdpt[pdpt_idx] |= VMM_USER;
        pd[pd_idx]     |= VMM_USER;
    }

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | flags | VMM_PRESENT;

    /* If any upper level was a large page that we split, the TLB may
     * still hold the old (possibly NX) mapping for the whole region.
     * A guaranteed full flush: temporarily clear CR4.PCIDE, reload
     * CR3 (always flushes when PCIDE=0), then restore PCIDE. */
    uint64_t cr3, cr4;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ull << 17)) { /* CR4.PCIDE */
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ull << 17)) : "memory");
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    } else {
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
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

uint64_t vmm_pte(uint64_t virt) {
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
    return pt[pt_idx];
}

uint64_t vmm_level_nx(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)pmm_phys_to_virt(g_cr3);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;

    uint64_t nx = 0;
    nx |= pml4[pml4_idx] & (1ull << 63);
    if (!(pml4[pml4_idx] & VMM_PRESENT)) {
        return nx;
    }
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
    nx |= pdpt[pdpt_idx] & (1ull << 63);
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        return nx;
    }
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    nx |= pd[pd_idx] & (1ull << 63);
    return nx;
}

uint64_t vmm_level_entry(uint64_t virt, int level) {
    uint64_t *pml4 = (uint64_t *)pmm_phys_to_virt(g_cr3);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;

    if (level == 3) {
        return pml4[pml4_idx];
    }
    if (!(pml4[pml4_idx] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (level == 2) {
        return pdpt[pdpt_idx];
    }
    if (!(pdpt[pdpt_idx] & VMM_PRESENT)) {
        return 0;
    }
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (level == 1) {
        return pd[pd_idx];
    }
    return 0;
}
