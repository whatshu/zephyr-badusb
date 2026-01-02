// SPDX-License-Identifier: Apache-2.0

#include "nologo_build.h"
#include "nologo_config.h"
#include "nologo_hid.h"
#include "nologo_printk_router.h"
#include "nologo_status_led.h"
#include "nologo_usb.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
    /* Status LED / pressure indicator (independent of USB). */
    nologo_status_init();

    /*
     * Debug build:
     * - Redirect printk to USB CDC and mirror to UART0.
     * Release build:
     * - Leave printk on UART0 only.
     */
    nologo_printk_router_init();

    printk("nologo_usb: boot (%s)\n", ((int)NOLOGO_DEBUG) ? "debug" : "release");

    /* Read /NAND:/config into memory at boot */
    (void)nologo_config_init();

    /* Register HID before enabling the USB stack */
    (void)nologo_hid_init();

    /* Start USB device stack (composite classes depend on build config) */
    (void)nologo_usb_init();

    /* main thread can idle forever; work is done by dedicated threads */
    k_sleep(K_FOREVER);
    return 0;
}


