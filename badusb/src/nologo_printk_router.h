// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Install the printk router hook (debug builds only). */
void nologo_printk_router_init(void);

/* Notifications from USB layer (safe to call in any build). */
void nologo_printk_router_on_usb_configured(void);
void nologo_printk_router_on_usb_dtr_set(void);


