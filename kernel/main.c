/*
 * main.c - TUS kernel entry point
 *
 * Boot flow:
 *   1. Limine (see limine.conf) switches the CPU to 64-bit long mode,
 *      maps this ELF at its linked higher-half addresses, sets up a
 *      stack and a flat GDT, and jumps to _start().
 *   2. _start() collects the boot protocol responses into g_bootinfo.
 *   3. The console (serial + framebuffer), IDT, PIC and the keyboard
 *      driver are initialized.
 *   4. Interrupts are enabled and control passes to the TUS shell.
 */

#include <limine.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "boot/splash.h"
#include "core/bootinfo.h"
#include "core/console.h"
#include "core/klib.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "drivers/serial.h"
#include "elf/tus_elf.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "shell/tsh.h"
#include "vfs/rootfs.h"
#include "vfs/vfs.h"

/* ---- Limine boot protocol requests ----
 *
 * Each request is a static struct placed in the ".requests" section
 * (see kernel/linker.ld). Limine fills in the response pointer before
 * the kernel starts; the request structs themselves are left intact.
 */

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

/*
 * Base revision: tells Limine which protocol features we rely on.
 * Revision 2 means "higher-half kernel, HHDM offset, framebuffers
 * mapped in the higher half". Limine finds this symbol by name in the
 * kernel's symbol table.
 */
static volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(2);

/* Single source of truth about what the bootloader gave us. */
struct bootinfo g_bootinfo;

/* How long the boot splash (toasts + boot log) stays on screen
 * before the shell clears it, in milliseconds. */
#define BOOT_SPLASH_HOLD_MS 2500

static void fill_bootinfo(void) {
    g_bootinfo.bootloader_name = bootloader_info_request.response != NULL
        ? bootloader_info_request.response->name : NULL;
    g_bootinfo.bootloader_version = bootloader_info_request.response != NULL
        ? bootloader_info_request.response->version : NULL;

    g_bootinfo.framebuffer = (framebuffer_request.response != NULL &&
                              framebuffer_request.response->framebuffer_count > 0)
        ? framebuffer_request.response->framebuffers[0] : NULL;

    g_bootinfo.hhdm_offset = hhdm_request.response != NULL
        ? hhdm_request.response->offset : 0;

    /* CPU count from the MP feature (includes the BSP). TUS itself is
     * single-CPU for now; the count drives the boot splash (one toast
     * per core) and the banner. */
    g_bootinfo.cpu_count = (mp_request.response != NULL)
        ? mp_request.response->cpu_count : 1;

    /* rootfs.img, loaded by Limine as the first module. */
    if (module_request.response != NULL &&
        module_request.response->module_count > 0) {
        g_bootinfo.rootfs_module = module_request.response->modules[0];
    } else {
        g_bootinfo.rootfs_module = NULL;
    }

    uint64_t total = 0;
    if (memmap_request.response != NULL) {
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry *entry = memmap_request.response->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                total += entry->length;
            }
        }
    }
    g_bootinfo.usable_memory_bytes = total;
}

/* Park every application processor: the bootloader starts the APs and
 * they spin on their goto_address until we publish one. TUS does not
 * use the APs yet, so we hand them a trivial cli/hlt loop - they stay
 * out of the way (and out of the TCG's way) instead of burning cycles. */
static void ap_park(struct limine_mp_info *cpu) {
    (void)cpu;
    for (;;) {
        asm volatile("cli; hlt");
    }
}

static void park_aps(void) {
    if (mp_request.response == NULL) {
        return;
    }
    for (uint64_t i = 0; i < mp_request.response->cpu_count; i++) {
        struct limine_mp_info *cpu = mp_request.response->cpus[i];
        if (cpu == NULL) {
            continue;
        }
        /* The BSP's goto_address is unused by the bootloader; setting
         * it is harmless and keeps the loop simple. */
        __atomic_store_n(&cpu->goto_address, ap_park, __ATOMIC_SEQ_CST);
    }
}

static void print_boot_banner(void) {
    const char *name = g_bootinfo.bootloader_name
        ? g_bootinfo.bootloader_name : "unknown";
    const char *version = g_bootinfo.bootloader_version
        ? g_bootinfo.bootloader_version : "";

    console_set_color(0x00FFA040, 0x00000000); /* toast orange */
    console_write("Toasty Unix Software (TUS)\n");
    console_set_color(0x00E8E8E8, 0x00000000);
    console_write("\"Work everywhere, but work right.\"\n");
    console_write("------------------------------------------------\n");
    kprintf("bootloader   : %s %s\n", name, version);
    console_write("architecture : x86_64 (AMD64)\n");
    kprintf("cpu count    : %llu\n", (unsigned long long)g_bootinfo.cpu_count);
    kprintf("memory       : %llu MiB usable\n",
            (unsigned long long)(g_bootinfo.usable_memory_bytes / (1024 * 1024)));
    if (g_bootinfo.framebuffer != NULL) {
        kprintf("framebuffer  : %llux%llu, %u bpp, pitch %llu @ %p\n",
                (unsigned long long)g_bootinfo.framebuffer->width,
                (unsigned long long)g_bootinfo.framebuffer->height,
                g_bootinfo.framebuffer->bpp,
                (unsigned long long)g_bootinfo.framebuffer->pitch,
                g_bootinfo.framebuffer->address);
    } else {
        console_write("framebuffer  : unavailable (serial console only)\n");
    }
    console_write("serial       : COM1 @ 115200 8N1 (debug mirror)\n");
    console_write("keyboard     : PS/2 scancode set 1, IRQ1\n");
    kprintf("timer        : PIT 100 Hz (IRQ0)\n");
    uint64_t total_frames = 0, free_frames = 0;
    pmm_get_stats(&total_frames, &free_frames);
    kprintf("pmm          : %llu free / %llu frames (%llu MiB usable)\n",
            (unsigned long long)free_frames,
            (unsigned long long)total_frames,
            (unsigned long long)(g_bootinfo.usable_memory_bytes / (1024 * 1024)));
    console_write("vfs          : /dev/fb0 tty0 kbd0 serial0 null zero\n");
    console_write("------------------------------------------------\n");
    console_write("tsh ready. Type 'help' to list the commands.\n\n");
}

/* Kernel entry point. Limine provides the stack; never returns. */
void _start(void) {
    cli(); /* build a clean interrupt environment */

    fill_bootinfo();
    park_aps(); /* APs halt in cli/hlt until the (single-CPU) kernel */
    console_init(g_bootinfo.framebuffer);

    /* Own GDT (kernel + user segments + TSS) before any interrupt
     * can fire; IDT selectors point at 0x08. */
    gdt_init();
    idt_init();
    pic_init();

    /* Memory: physical frames, page tables, kernel heap. */
    pmm_init(memmap_request.response, g_bootinfo.hhdm_offset);
    vmm_init();
    kmalloc_init();

    /* Devices and services. */
    pit_init();
    kbd_init();
    vfs_init();

    /* Mount the root filesystem (rootfs.img, a Limine module): the
     * directory tree (/dev, /tmp, /etc, /boot) and the OS files
     * (user programs, motd, boot logo) come from the image, not from
     * hardcoded kernel code. Device nodes are registered afterwards,
     * once /dev exists. */
    if (g_bootinfo.rootfs_module != NULL) {
        vfs_mount_rootfs(g_bootinfo.rootfs_module->address,
                         g_bootinfo.rootfs_module->size);
    } else {
        console_write("rootfs       : not loaded (no Limine module)\n");
    }

    /* The scheduler: tsh becomes task 0; the PIT (IRQ0) drives
     * round-robin switching. This runs BEFORE the device nodes are
     * registered so the standard descriptors land in task 0's own
     * fd table (vfs_devices_init opens them into the current task). */
    sched_init();
    vfs_devices_init();

    /* Draw the boot splash: one toast per CPU, boot logs below. */
    splash_show(g_bootinfo.cpu_count);

    /* SSE for user programs: the C library uses SSE2 instructions
     * (memcpy, string ops, math). The scheduler now saves/restores
     * FPU state, so user SSE survives task switches. */
    cpu_enable_sse();

    print_boot_banner();

    sti();

    /* Keep the boot splash (toast logos + boot log) on screen for a
     * moment before the shell takes over and clears it. The pause is
     * preempt-disabled so the timer tick cannot switch tasks while we
     * are still inside kernel boot code; pit_tick() still advances
     * the counter, so the sleep terminates normally. */
    preempt_disable();
    timer_sleep_ms(BOOT_SPLASH_HOLD_MS);
    preempt_enable();

    tsh_run();

    /* tsh_run() never returns; this is just a safety net. */
    for (;;) {
        hlt();
    }
}
