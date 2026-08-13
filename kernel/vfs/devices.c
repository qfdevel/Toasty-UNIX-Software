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
#include "../sched/sched.h"
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

/* TUS termios: byte-exact layout of the musl x86_64 struct termios
 * (4 x tcflag_t + cc_t c_line + cc_t c_cc[32] + 2 x speed_t = 57
 * bytes). The kernel stores the whole blob and interprets only the
 * flags it implements: ICRNL (Enter arrives as \r in raw mode),
 * ICANON (legacy ESC = EOF for `cat`) and ECHO (echo typed input). */
struct tus_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t __c_ispeed;
    uint32_t __c_ospeed;
};

/* Linux ioctl request numbers for termios and window size (musl and
 * the TUS kernel agree on these). */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413

/* Termios flag bits the tty honours (Linux values, match musl). */
#define TUS_ICRNL  0x0100
#define TUS_ICANON 0x0002
#define TUS_ECHO   0x0008

/* Default canonical termios: BRKINT|ICRNL|IXON input, OPOST output,
 * CS8|CREAD|B38400 control, ISIG|ICANON|ECHO|ECHOE|ECHOK|IEXTEN
 * local, VMIN=1. */
static struct tus_termios g_termios = {
    .c_iflag = 0x0302,
    .c_oflag = 0x0001,
    .c_cflag = 0x10B1,
    .c_lflag = 0x803B,
    .c_line = 0,
    .c_cc = { [6] = 1 }, /* VMIN = 1 */
    .__c_ispeed = 0x1001, /* B38400 */
    .__c_ospeed = 0x1001,
};

/* Pending bytes of a multi-byte escape sequence (arrow keys etc.). */
static char g_tty_pend[8];
static int g_tty_pend_n;
static int g_tty_pend_i;

/* Translate a special key into the escape sequence a real terminal
 * would send, and queue its bytes. Returns the first byte. */
static char tty_special_byte(int code) {
    static const struct {
        const char *seq;
        int len;
    } map[] = {
        [KBD_KEY_UP]       = { "\x1b[A", 3 },
        [KBD_KEY_DOWN]     = { "\x1b[B", 3 },
        [KBD_KEY_RIGHT]    = { "\x1b[C", 3 },
        [KBD_KEY_LEFT]     = { "\x1b[D", 3 },
        [KBD_KEY_HOME]     = { "\x1b[H", 3 },
        [KBD_KEY_END]      = { "\x1b[F", 3 },
        [KBD_KEY_DELETE]   = { "\x1b[3~", 4 },
        [KBD_KEY_INSERT]   = { "\x1b[2~", 4 },
        [KBD_KEY_PAGE_UP]  = { "\x1b[5~", 4 },
        [KBD_KEY_PAGE_DOWN]= { "\x1b[6~", 4 },
    };
    if (code < 0 || code >= (int)(sizeof(map) / sizeof(map[0])) ||
        map[code].seq == NULL) {
        return 0;
    }
    for (int i = 0; i < map[code].len; i++) {
        g_tty_pend[i] = map[code].seq[i];
    }
    g_tty_pend_n = map[code].len;
    g_tty_pend_i = 1; /* first byte returned immediately */
    return g_tty_pend[0];
}

static long tty_read(void *priv, void *buf, size_t count, size_t pos) {
    (void)priv;
    (void)pos;
    if (count == 0) {
        return 0;
    }

    /* Drain any queued bytes of a multi-byte key sequence first. */
    if (g_tty_pend_i < g_tty_pend_n) {
        *(char *)buf = g_tty_pend[g_tty_pend_i++];
        if (g_tty_pend_i == g_tty_pend_n) {
            g_tty_pend_i = g_tty_pend_n = 0;
        }
        return 1;
    }

    struct task *cur = sched_current();
    long pid = cur != NULL ? cur->pid : 1;
    bool canon = (g_termios.c_lflag & TUS_ICANON) != 0;
    bool echo = (g_termios.c_lflag & TUS_ECHO) != 0;

    for (;;) {
        struct kbd_event ev = kbd_get_event_owned(pid);

        if (ev.type == KBD_EVENT_SPECIAL) {
            char b = tty_special_byte(ev.code);
            if (b == 0) {
                continue; /* unmapped key: drop */
            }
            *(char *)buf = b;
            return 1;
        }
        if (ev.type != KBD_EVENT_CHAR) {
            continue;
        }

        if (ev.c == 0x1B) {
            if (canon) {
                return 0; /* ESC ends the stream (lets `cat` exit) */
            }
            *(char *)buf = ev.c;
            return 1;
        }

        char c = ev.c;
        /* A real terminal sends CR for Enter; with ICRNL cleared
         * (raw mode) the byte must stay \r. Our keyboard produces
         * \n directly, so invert the translation. */
        if (c == '\n' && (g_termios.c_iflag & TUS_ICRNL) == 0) {
            c = '\r';
        }
        if (echo && c >= 0x20 && c != 0x7F) {
            console_putchar(c);
        }
        *(char *)buf = c;
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

static int tty_ioctl(void *priv, uint64_t request, void *arg) {
    (void)priv;
    switch (request) {
    case TCGETS:
        if (arg == NULL) {
            return -EINVAL;
        }
        memcpy(arg, &g_termios, sizeof(g_termios));
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (arg == NULL) {
            return -EINVAL;
        }
        memcpy(&g_termios, arg, sizeof(g_termios));
        return 0;
    case TIOCGWINSZ: {
        if (arg == NULL) {
            return -EINVAL;
        }
        int cols = 80, rows = 24;
        fb_get_grid(&cols, &rows);
        uint16_t ws[4] = { (uint16_t)rows, (uint16_t)cols, 0, 0 };
        memcpy(arg, ws, sizeof(ws));
        return 0;
    }
    default:
        return -ENOTTY;
    }
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
        if (ev.type == KBD_EVENT_SPECIAL) {
            continue;
        }
        if (ev.type != KBD_EVENT_CHAR) {
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
    static const struct file_ops tty_ops = { tty_read, tty_write, tty_ioctl };
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
