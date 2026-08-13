/*
 * bootinfo.h - information gathered from the Limine boot protocol
 *
 * main() fills this single structure early in boot; every subsystem
 * reads from it afterwards. Keeping it in one place makes the kernel's
 * view of the world easy to follow and extend.
 */

#ifndef TUS_CORE_BOOTINFO_H
#define TUS_CORE_BOOTINFO_H

#include <stdint.h>

#include <limine.h>

struct bootinfo {
    const char *bootloader_name;                 /* e.g. "Limine" */
    const char *bootloader_version;              /* e.g. "12.5.2"  */
    struct limine_framebuffer *framebuffer;      /* NULL if absent  */
    uint64_t usable_memory_bytes;                /* sum of usable RAM */
    uint64_t hhdm_offset;                        /* phys -> higher-half */
    uint64_t cpu_count;                          /* BSP + APs (MP feature) */
    struct limine_file *rootfs_module;           /* boot():/boot/rootfs.img */
};

extern struct bootinfo g_bootinfo;

#endif /* TUS_CORE_BOOTINFO_H */
