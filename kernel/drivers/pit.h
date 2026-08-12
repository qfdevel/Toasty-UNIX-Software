/*
 * pit.h - Programmable Interval Timer (8254, channel 0)
 *
 * Generates a periodic interrupt on IRQ0. TUS programs it for 100 Hz,
 * giving the kernel a monotonic tick counter and a blocking sleep.
 * (An APIC/LAPIC timer will replace this later; the API stays the same.)
 */

#ifndef TUS_DRIVERS_PIT_H
#define TUS_DRIVERS_PIT_H

#include <stdint.h>

/* Program channel 0 to 100 Hz and enable IRQ0. */
void pit_init(void);

/* Advance the tick counter; called from the scheduler's IRQ0 path. */
void pit_tick(void);

/* Number of ticks since boot (100 ticks = 1 second). */
uint64_t pit_ticks(void);

/* Milliseconds since boot. */
uint64_t pit_uptime_ms(void);

/* Block for `ms` milliseconds (interrupts must be enabled). */
void timer_sleep_ms(uint32_t ms);

#endif /* TUS_DRIVERS_PIT_H */
