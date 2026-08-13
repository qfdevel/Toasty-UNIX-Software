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
 *
 * Address spaces: the kernel boots in the root space (Limine's own
 * tables). Every user task gets a private space (vmm_space_clone):
 * a fresh PML4 whose kernel half (indices 256..511) is copied from
 * the root by reference, so all tasks share the same kernel mappings
 * (kernel image, HHDM, framebuffer, heap). The user half is private
 * per task. The scheduler switches CR3 together with the task.
 *
 * Kernel-half mappings are always applied to the ROOT tables: because
 * the kernel half is shared by reference, a mapping there is visible
 * from every address space. User-half mappings go to the CURRENT
 * space (g_cr3), which is the caller's own space.
 */

#include "vmm.h"

#include <stdbool.h>

#include "pmm.h"
#include "../core/klib.h"

#define PAGE_SIZE   4096
#define PAGE_SHIFT  12

#define PTE_ADDR_MASK 0x000ffffffffff000ull
#define PTE_FLAG_MASK 0xfff

/* Kernel half boundary: PML4 index 256 == address 0xffff800000000000. */
#define KERNEL_HALF_BASE 0xffff800000000000ull

/* Flags inherited when splitting a large page: everything except the
 * PS bit (bit 7, not valid on 4 KiB leaves) and NX (bit 63, cleared so
 * the resulting pages stay executable). */
#define SPLIT_FLAGS_MASK (0xFFFull & ~(1ull << 7) & ~(1ull << 63))

static uint64_t g_cr3;      /* physical address of the current PML4 */
static uint64_t g_root_cr3; /* physical address of the boot PML4 */

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

/*
 * Walk the page tables of `cr3` down to the 4 KiB leaf PTE for `virt`
 * and return a pointer to that entry. Intermediate tables are created
 * on demand when `allocate` is set; large pages are split into fresh
 * tables that reproduce the same mapping at finer granularity (Limine
 * maps large regions this way). `split_out` receives whether a large
 * page had to be split (the TLB may then hold stale mappings).
 *
 * NX (bit 63) is cleared on every upper level we touch so the region
 * stays executable unless the caller asks for NX on the leaf. When
 * `user` is set, the U/S bit is propagated to every upper level so
 * ring 3 can actually reach the page.
 */
static uint64_t *walk_pt(uint64_t cr3, uint64_t virt, bool allocate,
                         bool user, bool *split_out) {
    uint64_t *pml4 = (uint64_t *)pmm_phys_to_virt(cr3);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    bool split = false;

    if (pml4[pml4_idx] & (1ull << 7)) {
        /* 1 GiB page in PML4: repoint to a new PDPT. */
        uint64_t base = pml4[pml4_idx] & PTE_ADDR_MASK;
        uint64_t flags_l = pml4[pml4_idx] & SPLIT_FLAGS_MASK;
        uint64_t new_phys = pmm_alloc_frame();
        if (new_phys == 0) {
            return NULL;
        }
        uint64_t *new_pdpt = (uint64_t *)pmm_phys_to_virt(new_phys);
        memset(new_pdpt, 0, PAGE_SIZE);
        for (int i = 0; i < 512; i++) {
            new_pdpt[i] = (base + (uint64_t)i * 0x40000000ull) | flags_l;
        }
        pml4[pml4_idx] = new_phys | (pml4[pml4_idx] & 0xFFF);
        split = true;
    }

    uint64_t pdpt_phys = table_fetch(&pml4[pml4_idx], allocate);
    if (pdpt_phys == 0) {
        return NULL;
    }
    uint64_t *pdpt = (uint64_t *)pmm_phys_to_virt(pdpt_phys);

    if (pdpt[pdpt_idx] & (1ull << 7)) {
        /* 1 GiB page in PDPT: split into a new PD of 2 MiB pages. */
        uint64_t base = pdpt[pdpt_idx] & PTE_ADDR_MASK;
        uint64_t flags_l = (pdpt[pdpt_idx] & SPLIT_FLAGS_MASK) | (1ull << 7);
        uint64_t new_phys = pmm_alloc_frame();
        if (new_phys == 0) {
            return NULL;
        }
        uint64_t *new_pd = (uint64_t *)pmm_phys_to_virt(new_phys);
        memset(new_pd, 0, PAGE_SIZE);
        for (int i = 0; i < 512; i++) {
            new_pd[i] = (base + (uint64_t)i * 0x200000ull) | flags_l;
        }
        pdpt[pdpt_idx] = new_phys | (pdpt[pdpt_idx] & 0xFFF);
        split = true;
    }

    uint64_t pd_phys = table_fetch(&pdpt[pdpt_idx], allocate);
    if (pd_phys == 0) {
        return NULL;
    }
    uint64_t *pd = (uint64_t *)pmm_phys_to_virt(pd_phys);

    if (pd[pd_idx] & (1ull << 7)) {
        /* 2 MiB page in PD: split into a new PT of 4 KiB pages. */
        uint64_t base = pd[pd_idx] & PTE_ADDR_MASK;
        uint64_t flags_l = pd[pd_idx] & SPLIT_FLAGS_MASK;
        uint64_t new_phys = pmm_alloc_frame();
        if (new_phys == 0) {
            return NULL;
        }
        uint64_t *new_pt = (uint64_t *)pmm_phys_to_virt(new_phys);
        memset(new_pt, 0, PAGE_SIZE);
        for (int i = 0; i < 512; i++) {
            new_pt[i] = (base + (uint64_t)i * 0x1000ull) | flags_l;
        }
        pd[pd_idx] = new_phys | (pd[pd_idx] & 0xFFF);
        split = true;
    }

    uint64_t pt_phys = table_fetch(&pd[pd_idx], allocate);
    if (pt_phys == 0) {
        return NULL;
    }
    uint64_t *pt = (uint64_t *)pmm_phys_to_virt(pt_phys);

    /* Clear NX on every upper level we touch; propagate U/S if the
     * caller wants a user page. */
    pml4[pml4_idx] &= ~(1ull << 63);
    pdpt[pdpt_idx] &= ~(1ull << 63);
    pd[pd_idx]     &= ~(1ull << 63);
    if (user) {
        pml4[pml4_idx] |= VMM_USER;
        pdpt[pdpt_idx] |= VMM_USER;
        pd[pd_idx]     |= VMM_USER;
    }

    if (split_out != NULL) {
        *split_out = split;
    }
    return &pt[pt_idx];
}

void vmm_init(void) {
    g_cr3 = read_cr3();
    g_root_cr3 = g_cr3;
}

uint64_t vmm_root_cr3(void) {
    return g_root_cr3;
}

uint64_t vmm_space_clone(void) {
    uint64_t phys = pmm_alloc_frame();
    if (phys == 0) {
        return 0;
    }
    uint64_t *new_pml4 = (uint64_t *)pmm_phys_to_virt(phys);
    uint64_t *root_pml4 = (uint64_t *)pmm_phys_to_virt(g_root_cr3);
    memset(new_pml4, 0, PAGE_SIZE);
    /* Share the kernel half by reference; user half starts empty. */
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = root_pml4[i];
    }
    return phys;
}

void vmm_space_switch(uint64_t cr3) {
    if (cr3 == g_cr3) {
        return;
    }
    g_cr3 = cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

int vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys,
                    uint64_t flags) {
    if ((virt & (PAGE_SIZE - 1)) || (phys & (PAGE_SIZE - 1))) {
        return -1;
    }

    bool split = false;
    uint64_t *pte = walk_pt(cr3, virt, true,
                            (flags & VMM_USER) != 0, &split);
    if (pte == NULL) {
        return -1;
    }

    *pte = (phys & PTE_ADDR_MASK) | flags | VMM_PRESENT;

    /* If an upper level was a large page that we split, the TLB may
     * still hold the old (possibly NX) mapping for the whole region.
     * A full CR3 reload always flushes when PCIDE is off. Only bother
     * when the touched space is the one currently loaded. */
    if (split && cr3 == g_cr3) {
        uint64_t cr3_cur, cr4;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_cur));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        if (cr4 & (1ull << 17)) { /* CR4.PCIDE */
            __asm__ volatile("mov %0, %%cr4"
                             : : "r"(cr4 & ~(1ull << 17)) : "memory");
            __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_cur) : "memory");
            __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        } else {
            __asm__ volatile("mov %0, %%cr3" : : "r"(cr3_cur) : "memory");
        }
    }
    invlpg(virt);
    return 0;
}

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t cr3 = (virt >= KERNEL_HALF_BASE) ? g_root_cr3 : g_cr3;
    return vmm_map_page_in(cr3, virt, phys, flags);
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
    uint64_t cr3 = (virt >= KERNEL_HALF_BASE) ? g_root_cr3 : g_cr3;
    uint64_t *pte = walk_pt(cr3, virt, false, false, NULL);
    if (pte == NULL) {
        return;
    }
    *pte = 0;
    invlpg(virt);
}

uint64_t vmm_translate_in(uint64_t cr3, uint64_t virt) {
    uint64_t *pte = walk_pt(cr3, virt, false, false, NULL);
    if (pte == NULL || !(*pte & VMM_PRESENT)) {
        return 0;
    }
    return (*pte & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

uint64_t vmm_translate(uint64_t virt) {
    return vmm_translate_in(g_cr3, virt);
}

/*
 * Ensure the intermediate tables for [virt, virt+bytes) exist in the
 * ROOT space, without mapping any frames. Called at boot (kmalloc
 * init) before any task can exist: afterwards, runtime kernel-half
 * mappings only touch shared leaf tables, so every task's address
 * space always sees them (no per-space table creation in the kernel
 * half, which would silently split off a task's view).
 */
void vmm_reserve_tables(uint64_t virt, size_t bytes) {
    uint64_t first = virt & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (virt + bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t page = first; page < last; page += PAGE_SIZE) {
        bool split = false;
        (void)walk_pt(g_root_cr3, page, true, false, &split);
    }
}

uint64_t vmm_pte(uint64_t virt) {
    uint64_t *pte = walk_pt(g_cr3, virt, false, false, NULL);
    return pte != NULL ? *pte : 0;
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
