// SPDX-License-Identifier: Apache-2.0

#include "nologo_printk_router.h"

#include "nologo_build.h"
#include "nologo_status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#if NOLOGO_DEBUG && IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)

/* USB CDC ACM device from devicetree (created under &zephyr_udc0 in app.overlay) */
static const struct device *const usb_uart_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

/* printk -> USB CDC routing (buffered until DTR/configured) */
#define PRINTK_USB_RB_SIZE 2048
static uint8_t printk_usb_buf[PRINTK_USB_RB_SIZE];
static struct ring_buf printk_usb_rb;
static struct k_spinlock printk_usb_lock;

K_EVENT_DEFINE(printk_usb_ev);
#define PRINTK_USB_EVT_FLUSH BIT(0)

static atomic_t usb_dtr_set;
static atomic_t usb_configured;

static printk_hook_fn_t prev_printk_hook;

static int nologo_printk_hook(int c)
{
    nologo_status_mark_printk_activity();

    /* Default output: USB CDC (buffered). */
    k_spinlock_key_t key = k_spin_lock(&printk_usb_lock);
    (void)ring_buf_put(&printk_usb_rb, (const uint8_t *)&c, 1);
    k_spin_unlock(&printk_usb_lock, key);

    k_event_post(&printk_usb_ev, PRINTK_USB_EVT_FLUSH);

    /* Mirror all printk to UART console (prev hook). */
    if (prev_printk_hook) {
        (void)prev_printk_hook(c);
    }

    return c;
}

static void printk_usb_flush_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (1) {
        (void)k_event_wait(&printk_usb_ev, PRINTK_USB_EVT_FLUSH, true, K_FOREVER);

        /* Flush after DTR or after configuration (some tools don't set DTR). */
        if ((atomic_get(&usb_dtr_set) == 0 && atomic_get(&usb_configured) == 0) ||
            !device_is_ready(usb_uart_dev)) {
            continue;
        }

        while (1) {
            uint8_t chunk[64];
            uint32_t n;

            k_spinlock_key_t key = k_spin_lock(&printk_usb_lock);
            n = ring_buf_get(&printk_usb_rb, chunk, sizeof(chunk));
            k_spin_unlock(&printk_usb_lock, key);

            if (n == 0) {
                break;
            }

            for (uint32_t i = 0; i < n; i++) {
                uart_poll_out(usb_uart_dev, chunk[i]);
            }
        }
    }
}

K_THREAD_DEFINE(printk_usb_thread_id, 1024, printk_usb_flush_thread,
        NULL, NULL, NULL, 6, 0, 0);

void nologo_printk_router_init(void)
{
    ring_buf_init(&printk_usb_rb, sizeof(printk_usb_buf), printk_usb_buf);

    prev_printk_hook = __printk_get_hook();
    __printk_hook_install(nologo_printk_hook);
}

void nologo_printk_router_on_usb_configured(void)
{
    atomic_set(&usb_configured, 1);
    k_event_post(&printk_usb_ev, PRINTK_USB_EVT_FLUSH);
}

void nologo_printk_router_on_usb_dtr_set(void)
{
    atomic_set(&usb_dtr_set, 1);
    k_event_post(&printk_usb_ev, PRINTK_USB_EVT_FLUSH);
}

#else

void nologo_printk_router_init(void) {}
void nologo_printk_router_on_usb_configured(void) {}
void nologo_printk_router_on_usb_dtr_set(void) {}

#endif


