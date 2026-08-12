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

#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "core/bootinfo.h"
#include "core/console.h"
#include "core/klib.h"
#include "drivers/keyboard.h"
#include "drivers/serial.h"
#include "shell/tsh.h"

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

/*
 * Base revision: tells Limine which protocol features we rely on.
 * Revision 2 means "higher-half kernel, HHDM offset, framebuffers
 * mapped in the higher half". Limine finds this symbol by name in the
 * kernel's symbol table.
 */
static volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(2);

/* Single source of truth about what the bootloader gave us. */
struct bootinfo g_bootinfo;

static void fill_bootinfo(void) {
    g_bootinfo.bootloader_name = bootloader_info_request.response != NULL
        ? bootloader_info_request.response->name : NULL;
    g_bootinfo.bootloader_version = bootloader_info_request.response != NULL
        ? bootloader_info_request.response->version : NULL;

    g_bootinfo.framebuffer = (framebuffer_request.response != NULL &&
                              framebuffer_request.response->framebuffer_count > 0)
        ? framebuffer_request.response->framebuffers[0] : NULL;

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
    console_write("------------------------------------------------\n");
    console_write("tsh ready. Type 'help' to list the commands.\n\n");
}

/* Kernel entry point. Limine provides the stack; never returns. */
void _start(void) {
    cli(); /* build a clean interrupt environment */

    fill_bootinfo();
    console_init(g_bootinfo.framebuffer);

    idt_init();
    pic_init();

    kbd_init();

    print_boot_banner();

    sti();
    tsh_run();

    /* tsh_run() never returns; this is just a safety net. */
    for (;;) {
        hlt();
    }
}
