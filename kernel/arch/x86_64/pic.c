/*
 * pic.c - 8259A PIC initialization and masking
 *
 * The two chips are cascaded: the slave's INT line is wired to IRQ2 of
 * the master. During initialization all IRQs are masked; drivers call
 * pic_enable_irq() for exactly the lines they use.
 */

#include "pic.h"
#include "io.h"

#define ICW1_INIT       0x11 /* cascade mode, edge triggered, ICW4 needed */
#define ICW4_8086       0x01 /* 8086/88 mode */

#define MASTER_BASE     0x20 /* master IRQ0..7  -> vectors 0x20..0x27 */
#define SLAVE_BASE      0x28 /* slave  IRQ8..15 -> vectors 0x28..0x2F */

#define SLAVE_ON_IRQ2   0x04 /* master: slave present on IRQ2 */
#define SLAVE_CASCADE_ID 0x02 /* slave:  cascade identity */

static uint8_t g_master_mask = 0xFF; /* 1 = masked */
static uint8_t g_slave_mask  = 0xFF;

void pic_init(void) {
    /* ICW1: start initialization sequence on both chips. */
    outb(PIC1_CMD_PORT, ICW1_INIT);
    io_wait();
    outb(PIC2_CMD_PORT, ICW1_INIT);
    io_wait();

    /* ICW2: vector base for each chip. */
    outb(PIC1_DATA_PORT, MASTER_BASE);
    io_wait();
    outb(PIC2_DATA_PORT, SLAVE_BASE);
    io_wait();

    /* ICW3: cascade wiring. */
    outb(PIC1_DATA_PORT, SLAVE_ON_IRQ2);
    io_wait();
    outb(PIC2_DATA_PORT, SLAVE_CASCADE_ID);
    io_wait();

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA_PORT, ICW4_8086);
    io_wait();
    outb(PIC2_DATA_PORT, ICW4_8086);
    io_wait();

    /* Mask everything; drivers unmask what they need. */
    outb(PIC1_DATA_PORT, g_master_mask);
    outb(PIC2_DATA_PORT, g_slave_mask);
}

void pic_enable_irq(uint8_t irq) {
    if (irq < 8) {
        g_master_mask &= (uint8_t)~(1u << irq);
        outb(PIC1_DATA_PORT, g_master_mask);
    } else if (irq < 16) {
        g_slave_mask &= (uint8_t)~(1u << (irq - 8));
        outb(PIC2_DATA_PORT, g_slave_mask);
    }
}

void pic_disable_irq(uint8_t irq) {
    if (irq < 8) {
        g_master_mask |= (uint8_t)(1u << irq);
        outb(PIC1_DATA_PORT, g_master_mask);
    } else if (irq < 16) {
        g_slave_mask |= (uint8_t)(1u << (irq - 8));
        outb(PIC2_DATA_PORT, g_slave_mask);
    }
}

void pic_send_eoi(uint8_t irq) {
    /* A slave IRQ needs an EOI on both chips. */
    if (irq >= 8) {
        outb(PIC2_CMD_PORT, 0x20);
    }
    outb(PIC1_CMD_PORT, 0x20);
}
