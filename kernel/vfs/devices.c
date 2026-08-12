/*
 * devices.c - built-in device implementations
 *
 *   /dev/fb0     raw framebuffer: read/write pixels, ioctl for info/fill
 *   /dev/tty0    console terminal: write text, read keyboard (ESC = EOF)
 *   /dev/kbd0    keyboard: read one keypress at a time (ESC = EOF)
 *   /dev/serial0 COM1: write debug output
 *   /dev/null    write sink, read EOF
 *   /dev/zero    read zero bytes, write sink
 */

#include "devices.h"

#include "vfs.h"
#include "../core/console.h"
#include "../core/errno.h"
#include "../core/klib.h"
#include "../drivers/fb.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
/* ---- /dev/fb0 ---- */

static long fb_read(void *priv, void *buf, size_t count, size_t pos) {
    (void)priv;
    struct fb_device_info *info = (struct fb_device_info *)priv;
    if (pos >= info->pitch * info->height) {
        return 0;
    }
    if (count > info->pitch * info->height - pos) {
        count = info->pitch * info->height - pos;
    }
    memcpy(buf, (void *)(uintptr_t)info->address + pos, count);
    return (long)count;
}

static long fb_write(void *priv, const void *buf, size_t count, size_t pos) {
    struct fb_device_info *info = (struct fb_device_info *)priv;
    if (pos >= info->pitch * info->height) {
        return 0;
    }
    if (count > info->pitch * info->height - pos) {
        count = info->pitch * info->height - pos;
    }
    memcpy((void *)(uintptr_t)info->address + pos, buf, count);
    return (long)count;
}

static int fb_ioctl(void *priv, uint64_t request, void *arg) {
    struct fb_device_info *info = (struct fb_device_info *)priv;
    switch (request) {
    case FB_IOCTL_GET_INFO:
        if (arg == NULL) {
            return -EINVAL;
        }
        memcpy(arg, info, sizeof(*info));
        return 0;
    case FB_IOCTL_FILL:
        fb_fill(arg != NULL ? *(uint32_t *)arg : 0);
        return 0;
    default:
        return -ENOTTY;
    }
}

/* ---- /dev/tty0 ---- */

static long tty_read(void *priv, void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)pos;
    if (count == 0) {
        return 0;
    }
    for (;;) {
        struct kbd_event ev = kbd_get_event();
        if (ev.type == KBD_EVENT_SCROLL_UP) {
            console_scroll_page(1);
            continue;
        }
        if (ev.type == KBD_EVENT_SCROLL_DOWN) {
            console_scroll_page(-1);
            continue;
        }
        if (ev.c == 0x1B) { /* ESC ends the stream (lets `cat` exit) */
            return 0;
        }
        *(char *)buf = ev.c;
        return 1;
    }
}

static long tty_write(void *priv, const void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)pos;
    const char *p = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        console_putchar(p[i]);
    }
    return (long)count;
}

/* ---- /dev/kbd0 ---- */

static long kbd_read(void *priv, void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)pos;
    if (count == 0) {
        return 0;
    }
    for (;;) {
        struct kbd_event ev = kbd_get_event();
        if (ev.type == KBD_EVENT_SCROLL_UP) {
            console_scroll_page(1);
            continue;
        }
        if (ev.type == KBD_EVENT_SCROLL_DOWN) {
            console_scroll_page(-1);
            continue;
        }
        if (ev.c == 0x1B) {
            return 0;
        }
        *(char *)buf = ev.c;
        return 1;
    }
}

/* ---- /dev/serial0 ---- */

static long serial_write_dev(void *priv, const void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)pos;
    const char *p = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        serial_putchar(p[i]);
    }
    return (long)count;
}

/* ---- /dev/null ---- */

static long null_write(void *priv, const void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)buf;
    (void)pos;
    return (long)count; /* swallow everything */
}

static long null_read(void *priv, void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)buf;
    (void)count;
    (void)pos;
    return 0; /* EOF */
}

/* ---- /dev/zero ---- */

static long zero_read(void *priv, void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)pos;
    memset(buf, 0, count);
    return (long)count;
}

void devices_init(void) {
    static const struct file_ops fb_ops = { fb_read, fb_write, fb_ioctl };
    static const struct file_ops tty_ops = { tty_read, tty_write, NULL };
    static const struct file_ops kbd_ops = { kbd_read, NULL, NULL };
    static const struct file_ops serial_ops = { NULL, serial_write_dev, NULL };
    static const struct file_ops null_ops = { null_read, null_write, NULL };
    static const struct file_ops zero_ops = { zero_read, null_write, NULL };

    /* Snapshot the framebuffer description for /dev/fb0. */
    static struct fb_device_info fb_info;
    uint32_t w = 0, h = 0, bpp = 0;
    uint64_t pitch = 0;
    void *address = NULL;
    fb_get_info(&w, &h, &bpp, &pitch, &address);
    fb_info.width = w;
    fb_info.height = h;
    fb_info.bpp = bpp;
    fb_info.pitch = pitch;
    fb_info.address = (uint64_t)(uintptr_t)address;

    vfs_create_device("/dev/fb0", &fb_ops, &fb_info);
    vfs_create_device("/dev/tty0", &tty_ops, NULL);
    vfs_create_device("/dev/kbd0", &kbd_ops, NULL);
    vfs_create_device("/dev/serial0", &serial_ops, NULL);
    vfs_create_device("/dev/null", &null_ops, NULL);
    vfs_create_device("/dev/zero", &zero_ops, NULL);
}
