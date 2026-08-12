/*
 * kmalloc.h - kernel heap allocator
 *
 * A simple first-fit free-list allocator over the VMM kernel heap
 * region. Blocks are 16-byte aligned; each block carries a small
 * header. The arena grows on demand by mapping fresh frames from the
 * PMM, so the heap never needs a fixed size upfront.
 */

#ifndef TUS_MM_KMALLOC_H
#define TUS_MM_KMALLOC_H

#include <stddef.h>

/* Initialize the heap (free list empty, arena at VMM_KHEAP_BASE). */
void kmalloc_init(void);

/* Allocate `size` bytes (16-byte aligned). Returns NULL on failure. */
void *kmalloc(size_t size);

/* Release a block previously returned by kmalloc(). */
void kfree(void *ptr);

/* Resize a block, preserving the contents (min of old/new size). */
void *krealloc(void *ptr, size_t size);

/* Bytes currently mapped for the heap arena (for diagnostics). */
size_t kmalloc_arena_bytes(void);

#endif /* TUS_MM_KMALLOC_H */
