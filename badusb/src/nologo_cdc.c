// SPDX-License-Identifier: Apache-2.0

#include "nologo_cdc.h"
#include "nologo_status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#include <errno.h>
#include <string.h>

#if IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)

static const struct device *const cdc_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

static void cdc_demo_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    const char *hello_msg = "hello world\r\n";
    size_t msg_len = strlen(hello_msg);

    while (!device_is_ready(cdc_dev)) {
        k_msleep(100);
    }

    while (1) {
        for (size_t i = 0; i < msg_len; i++) {
            uart_poll_out(cdc_dev, hello_msg[i]);
        }
        nologo_status_blink_green();
        k_msleep(1000);
    }
}

K_THREAD_DEFINE(cdc_demo_thread_id, 1024, cdc_demo_thread,
                NULL, NULL, NULL, 7, 0, K_TICKS_FOREVER);

int nologo_cdc_init(void)
{
    if (!device_is_ready(cdc_dev)) {
        return -ENODEV;
    }
    k_thread_start(cdc_demo_thread_id);
    return 0;
}

#else

int nologo_cdc_init(void)
{
    printk("cdc: CDC ACM not enabled\n");
    return -ENOTSUP;
}

#endif

