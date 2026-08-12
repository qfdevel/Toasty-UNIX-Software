/*
 * commands.c - built-in tsh commands
 *
 * Each command is a plain function `int fn(int argc, char **argv)`
 * with argv[0] being the command name itself, exactly like a UNIX
 * shell. New commands are added by appending an entry to g_commands.
 */

#include "commands.h"

#include "tsh.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "../core/bootinfo.h"
#include "core/console.h"
#include "core/klib.h"
#include "drivers/fb.h"

#define MAX_ARGS 16

struct command {
    const char *name;        /* what the user types */
    const char *description; /* shown by `help` */
    int (*run)(int argc, char **argv);
};

static int cmd_help(int argc, char **argv);
static int cmd_echo(int argc, char **argv);
static int cmd_clear(int argc, char **argv);
static int cmd_ver(int argc, char **argv);
static int cmd_about(int argc, char **argv);
static int cmd_sysinfo(int argc, char **argv);
static int cmd_reboot(int argc, char **argv);
static int cmd_crash(int argc, char **argv);

static const struct command g_commands[] = {
    { "help",    "list available commands",      cmd_help },
    { "echo",    "print its arguments",          cmd_echo },
    { "clear",   "clear the screen",             cmd_clear },
    { "ver",     "show the kernel version",      cmd_ver },
    { "about",   "show TUS information",         cmd_about },
    { "sysinfo", "show system information",      cmd_sysinfo },
    { "reboot",  "restart the machine",          cmd_reboot },
    { "crash",   "raise a CPU exception (demo)", cmd_crash },
};

#define COMMAND_COUNT (sizeof(g_commands) / sizeof(g_commands[0]))

static int cmd_help(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Available commands:\n");
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        kprintf("  %-10s %s\n", g_commands[i].name, g_commands[i].description);
    }
    return 0;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            console_putchar(' ');
        }
        console_write(argv[i]);
    }
    console_putchar('\n');
    return 0;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_clear();
    return 0;
}

static int cmd_ver(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("TUS kernel 0.1.0, built with %s\n", __VERSION__);
    return 0;
}

static int cmd_about(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Toasty Unix Software (TUS)\n");
    console_write("\"Work everywhere, but work right.\"\n");
    console_write("Architecture: x86_64 (AMD64)\n");
    kprintf("Bootloader  : %s %s\n",
            g_bootinfo.bootloader_name ? g_bootinfo.bootloader_name : "unknown",
            g_bootinfo.bootloader_version ? g_bootinfo.bootloader_version : "");
    return 0;
}

static int cmd_sysinfo(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char vendor[13];
    char brand[49];
    cpu_get_vendor(vendor);
    cpu_get_brand(brand);
    kprintf("CPU vendor  : %s\n", vendor);
    kprintf("CPU model   : %s\n", brand);

    kprintf("Memory      : %llu MiB usable\n",
            (unsigned long long)(g_bootinfo.usable_memory_bytes / (1024 * 1024)));

    uint32_t width = 0, height = 0, bpp = 0;
    uint64_t pitch = 0;
    void *address = NULL;
    fb_get_info(&width, &height, &bpp, &pitch, &address);
    if (width > 0 && height > 0) {
        kprintf("Framebuffer : %ux%u, %u bpp, pitch %llu @ %p\n",
                width, height, bpp, (unsigned long long)pitch, address);
    } else {
        console_write("Framebuffer : none (serial console only)\n");
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Rebooting...\n");
    /* Ask the 8042 keyboard controller to pulse the reset line. */
    outb(0x64, 0xFE);
    /* If the reset never fires, stop here instead of continuing. */
    for (;;) {
        cli();
        hlt();
    }
    return 0;
}

static int cmd_crash(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_write("Raising invalid opcode (#UD)...\n");
    __asm__ volatile("ud2"); /* never reached */
    return 0;
}

/*
 * Tokenize the line into argv (space separated) and dispatch. The line
 * is copied first so the caller's buffer stays untouched.
 */
void command_execute(const char *line) {
    char buffer[TSH_LINE_MAX];
    char *argv[MAX_ARGS];
    int argc = 0;

    size_t len = strlen(line);
    if (len >= sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }
    memcpy(buffer, line, len);
    buffer[len] = '\0';

    char *p = buffer;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (argc < MAX_ARGS) {
            argv[argc++] = p;
        }
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    if (argc == 0) {
        return; /* empty line */
    }

    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(argv[0], g_commands[i].name) == 0) {
            g_commands[i].run(argc, argv);
            return;
        }
    }

    kprintf("tsh: %s: command not found\n", argv[0]);
}
