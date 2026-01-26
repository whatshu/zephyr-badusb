// SPDX-License-Identifier: Apache-2.0

#include "nologo_build.h"
#include "nologo_cdc.h"
#include "nologo_config.h"
#include "nologo_hid.h"
#include "nologo_status_led.h"
#include "nologo_usb.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
    /* Status LED / pressure indicator (independent of USB). */
    nologo_status_init();

    printk("nologo_usb: boot (%s)\n", ((int)NOLOGO_DEBUG) ? "debug" : "release");

    /* Read /NAND:/config into memory at boot */
    (void)nologo_config_init();

    /* Register HID before enabling the USB stack */
    (void)nologo_hid_init();

    /* Start USB device stack (composite classes depend on build config) */
    (void)nologo_usb_init();

    /* Start CDC serial demo thread (sends "hello world" continuously) */
    (void)nologo_cdc_init();

    /* Initialization complete: turn off RED LED */
    nologo_status_init_done();

    /* main thread can idle forever; work is done by dedicated threads */
    k_sleep(K_FOREVER);
    return 0;
}


