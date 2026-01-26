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
    nologo_status_init();
    (void)nologo_config_init();
    (void)nologo_hid_init();
    (void)nologo_usb_init();
    (void)nologo_cdc_init();
    nologo_status_init_done();

    k_sleep(K_FOREVER);
    return 0;
}


