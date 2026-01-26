// SPDX-License-Identifier: Apache-2.0

#include "nologo_usb.h"
#include "nologo_cdc.h"

#include "nologo_status_led.h"

#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/uart.h>
#include <errno.h>
#include <string.h>

#if IS_ENABLED(CONFIG_USBD_MSC_CLASS)
#include <zephyr/usb/class/usbd_msc.h>
/* Flash-backed MSC LUN (disk-name "NAND" from devicetree msc_disk0) */
USBD_DEFINE_MSC_LUN(nand, "NAND", "nologo", "FlashDisk", "0.00");
#endif

/* USB device configuration (VID/PID are for local bring-up; change as needed) */
#define NOLOGO_USBD_VID 0x2fe3
#define NOLOGO_USBD_PID 0x0001

USBD_DEVICE_DEFINE(nologo_usbd,
           DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
           NOLOGO_USBD_VID, NOLOGO_USBD_PID);

USBD_DESC_LANG_DEFINE(nologo_lang);
USBD_DESC_MANUFACTURER_DEFINE(nologo_mfr, "nologo");
USBD_DESC_PRODUCT_DEFINE(nologo_product, "RP2040 nologo");

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(nologo_fs_config, 0, 250, &fs_cfg_desc);

static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
    /* VBUS gating (if supported) */
    if (usbd_can_detect_vbus(ctx)) {
        if (msg->type == USBD_MSG_VBUS_READY) {
            int rc = usbd_enable(ctx);
            if (rc != 0 && rc != -EALREADY) {
                printk("USBD: enable failed (%d: %s)\n", rc, strerror(-rc));
                nologo_status_set_fault();
            }
        }

        if (msg->type == USBD_MSG_VBUS_REMOVED) {
            int rc = usbd_disable(ctx);
            if (rc != 0 && rc != -EALREADY) {
                printk("USBD: disable failed (%d: %s)\n", rc, strerror(-rc));
                nologo_status_set_fault();
            }
        }
    }

#if IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)
    /* Handle CDC ACM control line state changes (DTR/RTS) */
    if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
        uint32_t dtr = 0;

        uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
        printk("usb: CDC ACM control line state changed, DTR=%u\n", dtr);

        if (dtr) {
            nologo_cdc_notify_dtr_set();
        } else {
            nologo_cdc_notify_dtr_clear();
        }
    }

    if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
        uint32_t baudrate = 0;

        uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_BAUD_RATE, &baudrate);
        printk("usb: CDC ACM line coding changed, baudrate=%u\n", baudrate);
    }
#endif
}

static int usb_stack_init(void)
{
    int err;

    err = usbd_add_descriptor(&nologo_usbd, &nologo_lang);
    if (err) {
        return err;
    }

    err = usbd_add_descriptor(&nologo_usbd, &nologo_mfr);
    if (err) {
        return err;
    }

    err = usbd_add_descriptor(&nologo_usbd, &nologo_product);
    if (err) {
        return err;
    }

    err = usbd_add_configuration(&nologo_usbd, USBD_SPEED_FS, &nologo_fs_config);
    if (err) {
        return err;
    }

    /* Register all enabled USB classes (CDC/MSC/HID as enabled by Kconfig) */
    err = usbd_register_all_classes(&nologo_usbd, USBD_SPEED_FS, 1, NULL);
    if (err) {
        return err;
    }

    /* Composite device with IAD */
    usbd_device_set_code_triple(&nologo_usbd, USBD_SPEED_FS,
                    USB_BCC_MISCELLANEOUS, 0x02, 0x01);

    err = usbd_msg_register_cb(&nologo_usbd, usb_msg_cb);
    if (err) {
        return err;
    }

    err = usbd_init(&nologo_usbd);
    if (err) {
        return err;
    }

    /* Enable immediately if VBUS detection not available */
    if (!usbd_can_detect_vbus(&nologo_usbd)) {
        err = usbd_enable(&nologo_usbd);
        if (err) {
            return err;
        }
    }

    return 0;
}

int nologo_usb_init(void)
{
#if IS_ENABLED(CONFIG_USBD_MSC_CLASS)
    /*
     * Debug builds expose MSC by default. Ensure the underlying disk is ready
     * before USB enumeration.
     */
    int rc = disk_access_init("NAND");
    if (rc != 0) {
        printk("msc: disk_access_init(\"NAND\") failed (%d: %s)\n", rc, strerror(-rc));
        nologo_status_set_fault();
    }
#endif

    int ret = usb_stack_init();
    if (ret != 0) {
        printk("usb: init failed (%d: %s)\n", ret, strerror(-ret));
        nologo_status_set_fault();
    }
    return ret;
}


