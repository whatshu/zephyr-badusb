// SPDX-License-Identifier: Apache-2.0

#include "nologo_status_led.h"
#include "nologo_build.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys_clock.h>
#include <zephyr/sys/printk.h>
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
 * CPU load measurement using thread runtime stats.
 * cpu_load_permille: 0 = idle, 1000 = 100% load
 */
static atomic_t cpu_load_permille;
static uint64_t last_all_cycles;
static uint64_t last_idle_cycles;

#if NOLOGO_DEBUG
/* Print CPU load every N ticks (100ms each) -> 1 second */
#define CPU_LOAD_PRINT_INTERVAL 10
static uint32_t cpu_load_print_counter;
#endif

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

static uint32_t cpu_load_to_pulse_off_ms(uint32_t load_permille)
{
    /*
     * Map CPU load (permille, 0-1000) -> OFF time (ms) between green pulses.
     *
     * - idle (0-5%):   stay at max OFF time (slow blink)
     * - moderate:      gradual decrease
     * - high (>80%):   fast blink
     */
    const uint32_t off_min = 100;   /* high load: ~10 blinks/sec */
    const uint32_t off_max = 3000;  /* idle: one pulse every ~3s */

    /* Dead zone: ignore load below 5% (50 permille) */
    const uint32_t dead_zone = 50U;
    /* Cap: beyond 80% (800 permille), we're at max indication */
    const uint32_t load_cap = 800U;

    if (load_permille <= dead_zone) {
        return off_max;
    }

    uint32_t effective_load = load_permille - dead_zone;
    uint32_t range = load_cap - dead_zone;

    /* Linear mapping with clamping */
    uint32_t p = MIN(effective_load, range);
    uint32_t off_range = off_max - off_min;

    return off_max - (uint32_t)(((uint64_t)p * off_range) / range);
}

static void cpu_load_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /*
     * Calculate CPU load using thread runtime stats.
     * We measure the idle thread's execution time vs total time.
     * CPU load = 1 - (idle_time / total_time)
     */
    k_thread_runtime_stats_t all_stats;
    k_thread_runtime_stats_t idle_stats;

    if (k_thread_runtime_stats_all_get(&all_stats) != 0) {
        k_event_post(&status_events, APP_EVT_PRESSURE_TICK);
        return;
    }

    /* Use all_stats for total cycles; idle cycles from idle thread */
    uint64_t curr_all = all_stats.execution_cycles;

    /* Try to get idle thread stats - thread 0 is typically idle */
    extern struct k_thread z_idle_threads[];
    if (k_thread_runtime_stats_get(&z_idle_threads[0], &idle_stats) != 0) {
        /* Fallback: assume no idle if we can't get stats */
        idle_stats.execution_cycles = 0;
    }
    uint64_t curr_idle = idle_stats.execution_cycles;

    if (last_all_cycles > 0) {
        uint64_t delta_all = curr_all - last_all_cycles;
        uint64_t delta_idle = curr_idle - last_idle_cycles;

        if (delta_all > 0) {
            /* Calculate load in permille (0-1000) */
            uint32_t load;
            if (delta_idle >= delta_all) {
                load = 0; /* All time spent in idle */
            } else {
                load = (uint32_t)(((delta_all - delta_idle) * 1000ULL) / delta_all);
            }

            /* EWMA: alpha=1/8 for responsive but smooth updates */
            uint32_t old_load = atomic_get(&cpu_load_permille);
            uint32_t new_load = (old_load * 7U + load) / 8U;
            atomic_set(&cpu_load_permille, new_load);
        }
    }

    last_all_cycles = curr_all;
    last_idle_cycles = curr_idle;

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

K_TIMER_DEFINE(cpu_load_timer, cpu_load_timer_handler, NULL);
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
            /* Infer printk "end" by inactivity (>150ms since last printk) */
            if ((atomic_get(&status_flags) & STATUS_FLAG_PRINT_ACTIVE) != 0) {
                if ((now_ms - last_printk_ms) > 150U) {
                    atomic_and(&status_flags, ~STATUS_FLAG_PRINT_ACTIVE);
                    k_event_post(&status_events, APP_EVT_STATUS_UPDATE);
                }
            }

            /* Update pulse timing from CPU load measurement */
            uint32_t load = (uint32_t)atomic_get(&cpu_load_permille);
            green_off_ms = cpu_load_to_pulse_off_ms(load);

#if NOLOGO_DEBUG
            /* Print CPU load periodically (every 1 second) */
            cpu_load_print_counter++;
            if (cpu_load_print_counter >= CPU_LOAD_PRINT_INTERVAL) {
                cpu_load_print_counter = 0;
                printk("CPU: %u.%u%%\n", load / 10, load % 10);
            }
#endif
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

    /* Periodic CPU load measurement tick (every 100ms for stable readings) */
    k_timer_start(&cpu_load_timer, K_MSEC(100), K_MSEC(100));
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


