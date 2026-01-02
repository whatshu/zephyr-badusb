// SPDX-License-Identifier: Apache-2.0

#include "nologo_status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys_clock.h>
#include <limits.h>

/* WS2812 LED strip (alias: led-strip) */
static const struct device *const led_strip = DEVICE_DT_GET(DT_ALIAS(led_strip));

/* Status LED (single RGB pixel): green=pressure blink, blue=printk active, red=fault */
#define STATUS_FLAG_FAULT        BIT(0)
#define STATUS_FLAG_PRINT_ACTIVE BIT(1)
static atomic_t status_flags;

static bool green_on;
static uint32_t green_on_ms = 20;
static uint32_t green_off_ms = 2000; /* idle: one pulse every ~2s */
static uint32_t last_led_apply_ms;

/* Track printk activity; "end" inferred by inactivity timeout */
static volatile uint32_t last_printk_ms;

/*
 * Pressure estimate: scheduling lateness (EWMA) in microseconds.
 * Measured directly in the timer ISR for accuracy.
 */
static uint64_t pressure_expected_cyc;
static atomic_t pressure_avg_late_us;

/*
 * Minimum latency threshold (us): ignore jitter below this level.
 * Typical context switch overhead on RP2040 is ~5-15us.
 */
#define PRESSURE_NOISE_FLOOR_US 50U

/* Application events */
#define APP_EVT_PRESSURE_TICK  BIT(0)
#define APP_EVT_GREEN_TOGGLE   BIT(1)
#define APP_EVT_STATUS_UPDATE  BIT(2)
K_EVENT_DEFINE(status_events);

static void status_led_apply(void)
{
    if (!device_is_ready(led_strip)) {
        return;
    }

    /* Rate limit updates to avoid hammering PIO if printk is very chatty */
    uint32_t now = k_uptime_get_32();
    if ((now - last_led_apply_ms) < 10U) {
        return;
    }
    last_led_apply_ms = now;

    atomic_val_t flags = atomic_get(&status_flags);

    struct led_rgb px = {0};
    if (green_on) {
        px.g = 0x10;
    }
    if ((flags & STATUS_FLAG_PRINT_ACTIVE) != 0) {
        px.b = 0x10;
    }
    if ((flags & STATUS_FLAG_FAULT) != 0) {
        px.r = 0x10;
    }

    (void)led_strip_update_rgb(led_strip, &px, 1);
}

static uint32_t pressure_to_pulse_off_ms(uint32_t avg_late_us)
{
    /*
     * Map EWMA lateness (us) -> OFF time (ms) between green pulses.
     *
     * - idle (0-200us):  stay at max OFF time (slow blink)
     * - moderate (200-2000us): gradual decrease
     * - high load (>2000us): fast blink
     *
     * The curve is designed so that normal system jitter doesn't cause
     * visible changes in blink rate. Only sustained load triggers faster
     * blinking.
     */
    const uint32_t off_min = 150;   /* high load: ~6 blinks/sec */
    const uint32_t off_max = 3000;  /* idle: one pulse every ~3s */

    /* Dead zone: ignore lateness below this threshold */
    const uint32_t dead_zone_us = 200U;
    /* Cap: beyond this, we're at max pressure indication */
    const uint32_t late_cap_us = 2000U;

    if (avg_late_us <= dead_zone_us) {
        return off_max;
    }

    uint32_t effective_late = avg_late_us - dead_zone_us;
    uint32_t range = late_cap_us - dead_zone_us;

    /* Linear mapping with clamping */
    uint32_t p = MIN(effective_late, range);
    uint32_t off_range = off_max - off_min;

    return off_max - (uint32_t)(((uint64_t)p * off_range) / range);
}

static void pressure_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /*
     * Measure lateness directly in ISR context for accuracy.
     * This reflects true timer/interrupt scheduling delays, not thread delays.
     */
    uint64_t now_cyc = k_cycle_get_64();
    uint64_t cps = sys_clock_hw_cycles_per_sec();
    uint64_t tcyc = (cps * 20ULL) / 1000ULL; /* 20ms tick in cycles */

    if (pressure_expected_cyc == 0U) {
        /* First tick: just initialize */
        pressure_expected_cyc = now_cyc + tcyc;
    } else {
        uint64_t late_cyc = (now_cyc > pressure_expected_cyc) ?
                            (now_cyc - pressure_expected_cyc) : 0U;

        /* Resync if paused/debugged (late > 0.5s) */
        if (late_cyc > (cps / 2ULL)) {
            pressure_expected_cyc = now_cyc + tcyc;
            late_cyc = 0U;
        } else {
            pressure_expected_cyc += tcyc;
        }

        uint32_t late_us = (uint32_t)MIN(
            (late_cyc * 1000000ULL) / cps,
            (uint64_t)UINT32_MAX);

        /* Apply noise floor to filter out normal jitter */
        if (late_us < PRESSURE_NOISE_FLOOR_US) {
            late_us = 0U;
        }

        /* EWMA: alpha=1/16 for smoother response */
        uint32_t old_avg = atomic_get(&pressure_avg_late_us);
        uint32_t new_avg = (old_avg * 15U + late_us) / 16U;
        atomic_set(&pressure_avg_late_us, new_avg);
    }

    k_event_post(&status_events, APP_EVT_PRESSURE_TICK);
}

/* Forward declaration: K_TIMER_DEFINE(green_timer, ...) is below */
extern struct k_timer green_timer;

static void green_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    green_on = !green_on;
    k_event_post(&status_events, APP_EVT_GREEN_TOGGLE);

    /* One-shot re-arm with current timing */
    if (green_on) {
        k_timer_start(&green_timer, K_MSEC(green_on_ms), K_NO_WAIT);
    } else {
        k_timer_start(&green_timer, K_MSEC(green_off_ms), K_NO_WAIT);
    }
}

K_TIMER_DEFINE(pressure_timer, pressure_timer_handler, NULL);
K_TIMER_DEFINE(green_timer, green_timer_handler, NULL);

static void status_led_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (1) {
        uint32_t events = k_event_wait(&status_events,
                          APP_EVT_PRESSURE_TICK |
                              APP_EVT_GREEN_TOGGLE |
                              APP_EVT_STATUS_UPDATE,
                          true, K_FOREVER);

        uint32_t now_ms = k_uptime_get_32();

        if (events & APP_EVT_PRESSURE_TICK) {
            /* Infer printk "end" by inactivity */
            if ((atomic_get(&status_flags) & STATUS_FLAG_PRINT_ACTIVE) != 0) {
                if ((now_ms - last_printk_ms) > 30U) {
                    atomic_and(&status_flags, ~STATUS_FLAG_PRINT_ACTIVE);
                    k_event_post(&status_events, APP_EVT_STATUS_UPDATE);
                }
            }

            /* Update pulse timing from ISR-measured pressure */
            green_off_ms = pressure_to_pulse_off_ms(
                (uint32_t)atomic_get(&pressure_avg_late_us));
        }

        if (events & (APP_EVT_GREEN_TOGGLE | APP_EVT_STATUS_UPDATE)) {
            status_led_apply();
        }
    }
}

K_THREAD_DEFINE(status_led_thread_id, 1024, status_led_thread,
        NULL, NULL, NULL, 7, 0, 0);

void nologo_status_init(void)
{
    /* Startup proof-of-life pulse */
    green_on = true;
    status_led_apply();
    k_timer_start(&green_timer, K_MSEC(green_on_ms), K_NO_WAIT);

    /* Periodic pressure tick */
    k_timer_start(&pressure_timer, K_NO_WAIT, K_MSEC(20));
    k_event_post(&status_events, APP_EVT_STATUS_UPDATE);
}

void nologo_status_set_fault(void)
{
    atomic_or(&status_flags, STATUS_FLAG_FAULT);
    k_event_post(&status_events, APP_EVT_STATUS_UPDATE);
}

void nologo_status_mark_printk_activity(void)
{
    last_printk_ms = k_uptime_get_32();

    atomic_val_t old = atomic_or(&status_flags, STATUS_FLAG_PRINT_ACTIVE);
    if ((old & STATUS_FLAG_PRINT_ACTIVE) == 0) {
        k_event_post(&status_events, APP_EVT_STATUS_UPDATE);
    }
}


