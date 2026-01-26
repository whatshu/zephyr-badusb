// SPDX-License-Identifier: Apache-2.0

#include "nologo_hid.h"

#include "nologo_build.h"
#include "nologo_status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <errno.h>
#include <string.h>

/* USB HID keyboard device */
static const struct device *const hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

enum {
    KB_MOD_KEY = 0,
    KB_RESERVED,
    KB_KEY_CODE1,
    KB_KEY_CODE2,
    KB_KEY_CODE3,
    KB_KEY_CODE4,
    KB_KEY_CODE5,
    KB_KEY_CODE6,
    KB_REPORT_SIZE,
};

UDC_STATIC_BUF_DEFINE(kbd_report, KB_REPORT_SIZE);
static volatile bool hid_ready;

/*
 * Keep the current "send 'a' once at boot" behavior, but allow the host to
 * change the keycode later via SET_REPORT (simple, future-proof hook).
 */
static atomic_t startup_keycode = ATOMIC_INIT(HID_KEY_A);

static void hid_iface_ready(const struct device *dev, const bool ready)
{
    ARG_UNUSED(dev);
    hid_ready = ready;
    printk("hid: iface %s\n", ready ? "ready" : "not ready");
}

static int hid_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
              const uint16_t len, uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
    return 0;
}

static int hid_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
              const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);

    /*
     * Host sends SET_REPORT for LED indicators (NumLock, CapsLock, etc.).
     * We don't need to handle them for this demo.
     */
    return 0;
}

static void hid_set_idle(const struct device *dev, const uint8_t id, const uint32_t duration)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    ARG_UNUSED(duration);
}

static uint32_t hid_get_idle(const struct device *dev, const uint8_t id)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    return 0;
}

static void hid_set_protocol(const struct device *dev, const uint8_t proto)
{
    ARG_UNUSED(dev);
    printk("hid: protocol %s\n", proto == 0U ? "boot" : "report");
}

static void hid_output_report(const struct device *dev, const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
}

static const struct hid_device_ops hid_kbd_ops = {
    .iface_ready = hid_iface_ready,
    .get_report = hid_get_report,
    .set_report = hid_set_report,
    .set_idle = hid_set_idle,
    .get_idle = hid_get_idle,
    .set_protocol = hid_set_protocol,
    .output_report = hid_output_report,
};

static void hid_demo_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    printk("hid: demo thread waiting for interface ready\n");

    while (!hid_ready) {
        k_msleep(50);
    }

    /*
     * Wait for USB enumeration to fully stabilize before sending
     * the first HID report. This helps avoid "Endpoint busy" errors
     * that can occur if the host is still setting up the interface.
     */
    k_msleep(500);

    printk("hid: sending demo key (keycode=0x%02x)\n",
           (uint8_t)atomic_get(&startup_keycode));

    /* Key press */
    memset(kbd_report, 0, KB_REPORT_SIZE);
    kbd_report[KB_KEY_CODE1] = (uint8_t)atomic_get(&startup_keycode);
    int ret = hid_device_submit_report(hid_dev, KB_REPORT_SIZE, kbd_report);
    if (ret == 0) {
        nologo_status_blink_blue();
    } else {
        printk("hid: key press submit failed (%d: %s)\n", ret, strerror(-ret));
    }

    k_msleep(50);

    /* Key release */
    memset(kbd_report, 0, KB_REPORT_SIZE);
    ret = hid_device_submit_report(hid_dev, KB_REPORT_SIZE, kbd_report);
    if (ret == 0) {
        nologo_status_blink_blue();
    } else {
        printk("hid: key release submit failed (%d: %s)\n", ret, strerror(-ret));
    }

    printk("hid: demo key sent\n");
    k_sleep(K_FOREVER);
}

K_THREAD_DEFINE(hid_demo_thread_id, 1024, hid_demo_thread,
                NULL, NULL, NULL, 7, 0, K_TICKS_FOREVER);

int nologo_hid_init(void)
{
    int ret;

    if (!device_is_ready(hid_dev)) {
        printk("hid: device not ready\n");
        nologo_status_set_fault();
        return -ENODEV;
    }

    ret = hid_device_register(hid_dev, hid_report_desc, sizeof(hid_report_desc), &hid_kbd_ops);
    if (ret != 0) {
        printk("hid: register failed (%d: %s)\n", ret, strerror(-ret));
        nologo_status_set_fault();
        return ret;
    }

    /* Start the HID demo thread now that registration is complete */
    k_thread_start(hid_demo_thread_id);

    printk("hid: registered\n");
    return 0;
}


