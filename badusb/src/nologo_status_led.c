// SPDX-License-Identifier: Apache-2.0

#include "nologo_status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>

static const struct device *const led_strip = DEVICE_DT_GET(DT_ALIAS(led_strip));

#define STATUS_FLAG_INIT_DONE  BIT(0)
#define STATUS_FLAG_FAULT      BIT(1)
static atomic_t status_flags;

#define LED_BLINK_DURATION_MS   50
#define LED_FAULT_PERIOD_MS     500

static atomic_t blue_on;
static atomic_t green_on;
static atomic_t red_blink_phase;

static void blue_off_work_handler(struct k_work *work);
static void green_off_work_handler(struct k_work *work);
static void fault_blink_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(blue_off_work, blue_off_work_handler);
static K_WORK_DELAYABLE_DEFINE(green_off_work, green_off_work_handler);
static K_WORK_DELAYABLE_DEFINE(fault_blink_work, fault_blink_work_handler);

static void status_led_apply(void)
{
    if (!device_is_ready(led_strip)) {
        return;
    }

    atomic_val_t flags = atomic_get(&status_flags);
    struct led_rgb px = {0};

    if (atomic_get(&green_on)) {
        px.g = 0x10;
    }
    if (atomic_get(&blue_on)) {
        px.b = 0x10;
    }
    if ((flags & STATUS_FLAG_FAULT) != 0) {
        if (atomic_get(&red_blink_phase)) {
            px.r = 0x20;
        }
    } else if ((flags & STATUS_FLAG_INIT_DONE) == 0) {
        px.r = 0x10;
    }

    (void)led_strip_update_rgb(led_strip, &px, 1);
}

static void blue_off_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    atomic_set(&blue_on, 0);
    status_led_apply();
}

static void green_off_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    atomic_set(&green_on, 0);
    status_led_apply();
}

static void fault_blink_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    atomic_val_t flags = atomic_get(&status_flags);
    if ((flags & STATUS_FLAG_FAULT) == 0) {
        atomic_set(&red_blink_phase, 0);
        status_led_apply();
        return;
    }

    atomic_val_t phase = atomic_get(&red_blink_phase);
    atomic_set(&red_blink_phase, phase ? 0 : 1);
    status_led_apply();
    k_work_schedule(&fault_blink_work, K_MSEC(LED_FAULT_PERIOD_MS));
}

void nologo_status_init(void)
{
    atomic_clear(&status_flags);
    atomic_set(&blue_on, 0);
    atomic_set(&green_on, 0);
    atomic_set(&red_blink_phase, 0);
    status_led_apply();
}

void nologo_status_init_done(void)
{
    atomic_or(&status_flags, STATUS_FLAG_INIT_DONE);
    status_led_apply();
}

void nologo_status_set_fault(void)
{
    atomic_val_t old_flags = atomic_or(&status_flags, STATUS_FLAG_FAULT);
    if ((old_flags & STATUS_FLAG_FAULT) == 0) {
        atomic_set(&red_blink_phase, 1);
        status_led_apply();
        k_work_schedule(&fault_blink_work, K_MSEC(LED_FAULT_PERIOD_MS));
    }
}

void nologo_status_blink_blue(void)
{
    atomic_set(&blue_on, 1);
    status_led_apply();
    k_work_schedule(&blue_off_work, K_MSEC(LED_BLINK_DURATION_MS));
}

void nologo_status_blink_green(void)
{
    atomic_set(&green_on, 1);
    status_led_apply();
    k_work_schedule(&green_off_work, K_MSEC(LED_BLINK_DURATION_MS));
}

