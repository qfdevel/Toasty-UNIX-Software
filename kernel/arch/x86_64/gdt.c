/*
 * gdt.c - Global Descriptor Table and Task State Segment
 *
 * The kernel boots on Limine's GDT (CS=0x28, data=0x30). For user
 * mode we install our own table:
 *
 *     0x00  NULL
 *     0x08  kernel code  (ring 0, 64-bit, execute/read)
 *     0x10  kernel data  (ring 0, read/write)
 *     0x18  user code    (ring 3, 64-bit, execute/read)
 *     0x20  user data    (ring 3, read/write)
 *     0x28  TSS          (task state segment for ring-3->ring-0
 *                         stack switching; RSP0 points at the
 *                         current task's kernel stack)
 *
 * After loading the GDT the code segment is changed to 0x08 with a
 * far return, and the data segments are reloaded to 0x10. The TSS is
 * marked busy with LTR.
 */

#include "gdt.h"

#include "io.h"

/* ---- selector constants (must match the layout above) ---- */

#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_CODE   0x18
#define SEL_USER_DATA   0x20
#define SEL_TSS         0x28

/* ---- GDT entry ---- */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_hi;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* ---- TSS (104 bytes, AMD64 layout) ---- */

struct tss {
    uint32_t rsvd0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t rsvd1;
    uint64_t ist[7];
    uint64_t rsvd2;
    uint16_t rsvd3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt_entry g_gdt[8];
static struct gdt_ptr g_gdt_ptr;
static struct tss g_tss;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    g_gdt[i].limit_low    = (uint16_t)(limit & 0xFFFF);
    g_gdt[i].base_low     = (uint16_t)(base & 0xFFFF);
    g_gdt[i].base_mid     = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[i].access       = access;
    g_gdt[i].flags_limit_hi = (uint8_t)((flags << 4) | ((limit >> 16) & 0x0F));
    g_gdt[i].base_high    = (uint8_t)((base >> 24) & 0xFF);
}

/* Install the TSS descriptor (a 16-byte system descriptor spanning
 * two GDT slots: 0x28 and 0x30). */
static void gdt_set_tss(int i, uint64_t base, uint32_t limit) {
    g_gdt[i].limit_low    = (uint16_t)(limit & 0xFFFF);
    g_gdt[i].base_low     = (uint16_t)(base & 0xFFFF);
    g_gdt[i].base_mid     = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[i].access       = 0x89; /* present, 64-bit TSS, available */
    g_gdt[i].flags_limit_hi = (uint8_t)(((limit >> 16) & 0x0F) | 0x00);
    g_gdt[i].base_high    = (uint8_t)((base >> 24) & 0xFF);

    /* Second half of the 16-byte descriptor holds the top 32 bits of
     * the base (DW6 = base[63:32] low word, DW7 = high word). */
    uint32_t base_hi = (uint32_t)(base >> 32);
    g_gdt[i + 1].limit_low    = (uint16_t)(base_hi & 0xFFFF);
    g_gdt[i + 1].base_low     = (uint16_t)((base_hi >> 16) & 0xFFFF);
    g_gdt[i + 1].base_mid    = 0;
    g_gdt[i + 1].access       = 0;
    g_gdt[i + 1].flags_limit_hi = 0;
    g_gdt[i + 1].base_high    = 0;
}

/* Reload segment registers onto the new table and switch CS to 0x08. */
static void gdt_reload(void) {
    __asm__ volatile(
        "lgdt %0\n\t"
        /* Far return: push new CS and the next RIP, then lretq. */
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        :
        : "m"(g_gdt_ptr), "i"(SEL_KERNEL_CODE), "i"(SEL_KERNEL_DATA)
        : "rax", "memory");
}

void gdt_init(void) {
    /* Flat 4 GiB segments: base 0, limit 0xFFFFF with G=1 (4K
     * granularity) covers the whole 32-bit space; in 64-bit mode the
     * base/limit are mostly ignored for code/data segments. */
    gdt_set_entry(0, 0, 0, 0, 0);                       /* NULL */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA);            /* kernel code */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xA);            /* kernel data */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA);            /* user code */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xA);            /* user data */
    gdt_set_tss(5, (uint64_t)(uintptr_t)&g_tss, sizeof(g_tss) - 1);

    g_gdt_ptr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdt_ptr.base  = (uint64_t)(uintptr_t)g_gdt;

    gdt_reload();

    /* Load the TSS and mark it busy. */
    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)SEL_TSS));
}

/* Point TSS.RSP0 at a kernel stack; called on every task switch so
 * interrupts from user mode land on the current task's stack. */
void tss_set_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
}
