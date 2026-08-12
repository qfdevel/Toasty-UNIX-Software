/*
 * pit.c - PIT driver implementation
 *
 * Channel 0 is configured in mode 3 (square wave). The 1.19318 MHz
 * input clock divided by 11932 gives 100.0008 Hz - close enough for
 * a monotonic tick counter and coarse sleeps.
 */

#include "pit.h"

#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/io.h"
#include "../arch/x86_64/pic.h"

#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43

#define PIT_IRQ       0

#define DIVISOR_100HZ 11932

static volatile uint64_t g_ticks;

/* IRQ0 handler: advance the tick counter. Plain function, see
 * keyboard.c for the interrupt-attribute rule. */
static void pit_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    g_ticks++;
}

void pit_init(void) {
    /* Command: channel 0, lobyte/hibyte access, mode 3, binary. */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0_DATA, (uint8_t)(DIVISOR_100HZ & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((DIVISOR_100HZ >> 8) & 0xFF));

    irq_install(PIT_IRQ, pit_irq_handler);
    pic_enable_irq(PIT_IRQ);
}

uint64_t pit_ticks(void) {
    return g_ticks;
}

uint64_t pit_uptime_ms(void) {
    return g_ticks * 10;
}

void timer_sleep_ms(uint32_t ms) {
    uint64_t target = g_ticks + (uint64_t)(ms + 9) / 10;
    while (g_ticks < target) {
        hlt(); /* IRQ0 wakes us */
    }
}
