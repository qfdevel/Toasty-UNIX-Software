/*
 * kmalloc.c - kernel heap allocator implementation
 *
 * Block layout (all offsets relative to the block start):
 *
 *     +--------+-------------------+
 *     | header | user data         |
 *     +--------+-------------------+
 *
 * header = { size (payload, multiple of 16), next (free list link) }
 *
 * Allocation: first-fit over the free list; if a block is larger than
 * needed it is split into two. Freeing: the block is reinserted into
 * the list and merged with adjacent free blocks to fight fragmentation.
 *
 * When the free list is empty, the arena is grown by one or more
 * pages, which are mapped from fresh PMM frames.
 */

#include "kmalloc.h"

#include "pmm.h"
#include "vmm.h"
#include "../core/klib.h"

#define ALIGN 16
#define PAGE_SIZE 4096

struct block {
    size_t size;            /* payload size, multiple of ALIGN */
    size_t pages;           /* >0: multi-page block (arena-allocated) */
    struct block *next;     /* free list link (only when free) */
};

#define HEADER_SIZE (sizeof(struct block))

static struct block *g_free;         /* free list head */
static uint64_t g_arena_next;        /* next virtual address to map */
static size_t g_arena_bytes;         /* mapped bytes */

static size_t align_up(size_t n) {
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

void kmalloc_init(void) {
    g_free = NULL;
    g_arena_next = VMM_KHEAP_BASE;
    g_arena_bytes = 0;

    /* Pre-create the intermediate page tables for the whole heap
     * region in the root space. This runs before any user task can
     * exist; afterwards, growing the heap only writes shared leaf
     * PTEs, which every per-task address space sees. (Without this,
     * a table allocation triggered while a task's space is active
     * could end up private to that space and the heap would break.) */
    vmm_reserve_tables(VMM_KHEAP_BASE, VMM_KHEAP_SIZE);
}

/* Map one more 4 KiB frame and add it to the free list. */
static void heap_grow_page(void) {
    uint64_t phys = pmm_alloc_frame();
    if (phys == 0) {
        return; /* out of physical memory */
    }
    vmm_map_page(g_arena_next, phys, VMM_WRITE);

    struct block *b = (struct block *)g_arena_next;
    b->size = PAGE_SIZE - HEADER_SIZE;
    b->next = g_free;
    g_free = b;

    g_arena_next += PAGE_SIZE;
    g_arena_bytes += PAGE_SIZE;
}

static void heap_grow(size_t payload) {
    while (payload > 0) {
        heap_grow_page();
        payload -= payload > PAGE_SIZE ? PAGE_SIZE : payload;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) {
        size = ALIGN;
    }
    size = align_up(size);
    if (size > PAGE_SIZE - HEADER_SIZE) {
        /* Large allocation: reserve whole arena pages and never put
         * the block on the free list. The header records the page
         * count so kfree() can release the frames again. */
        size_t pages = (size + HEADER_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
        struct block *b = (struct block *)g_arena_next;
        for (size_t i = 0; i < pages; i++) {
            uint64_t phys = pmm_alloc_frame();
            if (phys == 0) {
                return NULL;
            }
            vmm_map_page(g_arena_next + i * PAGE_SIZE, phys, VMM_WRITE);
        }
        b->size = size;
        b->pages = pages;
        b->next = NULL;
        g_arena_next += pages * PAGE_SIZE;
        g_arena_bytes += pages * PAGE_SIZE;
        return (void *)((uint8_t *)b + HEADER_SIZE);
    }

    /* First-fit with splitting. */
    struct block **link = &g_free;
    while (*link != NULL) {
        struct block *b = *link;
        if (b->size >= size) {
            if (b->size >= size + HEADER_SIZE + ALIGN) {
                /* Split: the remainder becomes a new free block. */
                struct block *rest = (struct block *)((uint8_t *)b + HEADER_SIZE + size);
                rest->size = b->size - size - HEADER_SIZE;
                rest->next = b->next;
                *link = rest;
                b->size = size;
            } else {
                *link = b->next;
            }
            return (void *)((uint8_t *)b + HEADER_SIZE);
        }
        link = &b->next;
    }

    /* Out of free blocks: grow the arena and retry once. */
    size_t before = g_arena_bytes;
    heap_grow(size);
    if (g_arena_bytes == before) {
        return NULL; /* could not grow (out of physical memory) */
    }
    return kmalloc(size);
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    struct block *b = (struct block *)((uint8_t *)ptr - HEADER_SIZE);

    /* Multi-page block: release its frames back to the PMM. The page
     * count is read ONCE before the loop - the first iteration unmaps
     * the page that holds the header, so re-reading b->pages from
     * memory afterwards would fault. */
    if (b->pages > 0) {
        size_t pages = b->pages;
        uint64_t virt = (uint64_t)(uintptr_t)b;
        for (size_t i = 0; i < pages; i++) {
            uint64_t phys = vmm_translate(virt + i * PAGE_SIZE);
            if (phys != 0) {
                vmm_unmap_page(virt + i * PAGE_SIZE);
                pmm_free_frame(phys);
            }
        }
        return;
    }

    /* Insert sorted by address, merging with both neighbours. */
    struct block **link = &g_free;
    while (*link != NULL && *link < b) {
        link = &(*link)->next;
    }
    b->next = *link;
    *link = b;

    /* Merge with the successor (if adjacent). */
    if (b->next != NULL &&
        (uint8_t *)b + HEADER_SIZE + b->size == (uint8_t *)b->next) {
        b->size += HEADER_SIZE + b->next->size;
        b->next = b->next->next;
    }
    /* Merge with the predecessor (if adjacent). */
    if (*link != b) {
        struct block *prev = g_free;
        while (prev->next != b) {
            prev = prev->next;
        }
        if ((uint8_t *)prev + HEADER_SIZE + prev->size == (uint8_t *)b) {
            prev->size += HEADER_SIZE + b->size;
            prev->next = b->next;
        }
    }
}

void *krealloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return kmalloc(size);
    }
    struct block *b = (struct block *)((uint8_t *)ptr - HEADER_SIZE);
    size = align_up(size);
    if (size <= b->size) {
        return ptr; /* fits already */
    }
    void *fresh = kmalloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, b->size);
    kfree(ptr);
    return fresh;
}

size_t kmalloc_arena_bytes(void) {
    return g_arena_bytes;
}
