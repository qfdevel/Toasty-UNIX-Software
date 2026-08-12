/*
 * keyboard.c - PS/2 keyboard driver implementation
 *
 * Scancode set 1 layout: a make code (key pressed) is the bare scan
 * code, the matching break code (key released) has bit 7 set. Some
 * keys send a 0xE0 prefix byte first; those (arrows, etc.) are
 * currently ignored.
 *
 * Shift and Ctrl are tracked as state; Caps Lock toggles and updates
 * the keyboard LED. Character generation follows the classic US layout.
 */

#include "keyboard.h"

#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

#define KBD_IRQ         1

#define KBD_BUFFER_SIZE 256

#define SC_EXTENDED     0xE0 /* prefix of multi-byte scancodes */
#define SC_CTRL         0x1D
#define SC_LSHIFT       0x2A
#define SC_RSHIFT       0x36
#define SC_CAPS_LOCK    0x3A

/* ---- scancode set 1 -> ASCII (US layout), indexed by scancode ---- */

static const char kbd_normal[128] = {
    [0x01] = 0x1B, /* Escape */
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',
    [0x06] = '5',  [0x07] = '6',  [0x08] = '7',  [0x09] = '8',
    [0x0A] = '9',  [0x0B] = '0',  [0x0C] = '-',  [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',
    [0x14] = 't',  [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',
    [0x18] = 'o',  [0x19] = 'p',  [0x1A] = '[',  [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',
    [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',
    [0x26] = 'l',  [0x27] = ';',  [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',
    [0x2F] = 'v',  [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm',
    [0x33] = ',',  [0x34] = '.',  [0x35] = '/',
    [0x37] = '*',  /* keypad multiply */
    [0x39] = ' ',
};

static const char kbd_shifted[128] = {
    [0x02] = '!',  [0x03] = '@',  [0x04] = '#',  [0x05] = '$',
    [0x06] = '%',  [0x07] = '^',  [0x08] = '&',  [0x09] = '*',
    [0x0A] = '(',  [0x0B] = ')',  [0x0C] = '_',  [0x0D] = '+',
    [0x10] = 'Q',  [0x11] = 'W',  [0x12] = 'E',  [0x13] = 'R',
    [0x14] = 'T',  [0x15] = 'Y',  [0x16] = 'U',  [0x17] = 'I',
    [0x18] = 'O',  [0x19] = 'P',  [0x1A] = '{',  [0x1B] = '}',
    [0x1E] = 'A',  [0x1F] = 'S',  [0x20] = 'D',  [0x21] = 'F',
    [0x22] = 'G',  [0x23] = 'H',  [0x24] = 'J',  [0x25] = 'K',
    [0x26] = 'L',  [0x27] = ':',  [0x28] = '"',  [0x29] = '~',
    [0x2B] = '|',  [0x2C] = 'Z',  [0x2D] = 'X',  [0x2E] = 'C',
    [0x2F] = 'V',  [0x30] = 'B',  [0x31] = 'N',  [0x32] = 'M',
    [0x33] = '<',  [0x34] = '>',  [0x35] = '?',
    [0x37] = '*',  /* keypad multiply */
};

/* ---- keyboard state ---- */

static volatile char g_buffer[KBD_BUFFER_SIZE];
static volatile int g_head; /* next slot to write (IRQ context) */
static volatile int g_tail; /* next slot to read (shell context) */

static bool g_shift_pressed;
static bool g_ctrl_pressed;
static bool g_caps_locked;
static bool g_extended; /* set after a 0xE0 prefix byte */

/* Push one character into the ring buffer; drops on overflow. */
static void kbd_push(char c) {
    int next = (g_head + 1) % KBD_BUFFER_SIZE;
    if (next != g_tail) {
        g_buffer[g_head] = c;
        g_head = next;
    }
}

/*
 * Update the Caps Lock LED. The keyboard must acknowledge each command
 * byte with 0xFA; we wait for it with a bounded poll so that hardware
 * which never answers cannot hang the interrupt handler.
 */
static void kbd_update_leds(void) {
    outb(KBD_DATA_PORT, 0xED); /* set LEDs command */
    for (unsigned i = 0; i < 10000; i++) {
        if (inb(KBD_STATUS_PORT) & 0x01) {
            (void)inb(KBD_DATA_PORT); /* ACK */
            break;
        }
    }
    outb(KBD_DATA_PORT, g_caps_locked ? 0x04 : 0x00); /* Caps Lock LED */
    for (unsigned i = 0; i < 10000; i++) {
        if (inb(KBD_STATUS_PORT) & 0x01) {
            (void)inb(KBD_DATA_PORT); /* ACK */
            break;
        }
    }
}

/*
 * IRQ1 handler: decode one scancode and feed the ring buffer.
 *
 * NOTE: this is a plain C function, NOT an __attribute__((interrupt))
 * function. The interrupt attribute belongs only to the IDT stubs in
 * idt.c (irq_stub_N), which dispatch to us with the interrupt frame
 * passed as a normal argument. Marking a registered handler as
 * "interrupt" would make it return with IRETQ instead of RET, popping
 * garbage - an instant #GP.
 */
static void kbd_irq_handler(struct interrupt_frame *frame) {
    (void)frame;

    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode == SC_EXTENDED) {
        g_extended = true;
        return;
    }
    if (g_extended) {
        g_extended = false;
        return; /* extended keys (arrows, keypad) are not mapped yet */
    }

    bool make = !(scancode & 0x80);
    uint8_t code = scancode & 0x7F;

    /* Modifier keys update state and produce no character. */
    switch (code) {
    case SC_CTRL:
        g_ctrl_pressed = make;
        return;
    case SC_LSHIFT:
    case SC_RSHIFT:
        g_shift_pressed = make;
        return;
    case SC_CAPS_LOCK:
        if (make) {
            g_caps_locked = !g_caps_locked;
            kbd_update_leds();
        }
        return;
    default:
        break;
    }

    if (!make) {
        return; /* break codes of ordinary keys produce nothing */
    }

    char c = g_shift_pressed ? kbd_shifted[code] : kbd_normal[code];
    if (c == 0) {
        return; /* unmapped scancode */
    }

    /* Caps Lock inverts the case of letters, independently of the
     * shift-dependent table we already chose. */
    if (g_caps_locked && c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    } else if (g_caps_locked && c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }

    /* Ctrl + letter produces the corresponding control character. */
    if (g_ctrl_pressed && c >= 'a' && c <= 'z') {
        c = (char)(c & 0x1F);
    }

    kbd_push(c);
}

void kbd_init(void) {
    irq_install(KBD_IRQ, kbd_irq_handler);
    pic_enable_irq(KBD_IRQ);
}

char kbd_getchar(void) {
    /* Sleep until the keyboard IRQ wakes us with a new character. */
    while (g_head == g_tail) {
        hlt();
    }
    char c = g_buffer[g_tail];
    g_tail = (g_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

int kbd_poll(void) {
    if (g_head == g_tail) {
        return -1;
    }
    char c = g_buffer[g_tail];
    g_tail = (g_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

bool kbd_has_char(void) {
    return g_head != g_tail;
}
