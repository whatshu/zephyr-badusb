// SPDX-License-Identifier: Apache-2.0

#include "nologo_cdc.h"
#include "nologo_status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

#include <errno.h>
#include <string.h>

#if IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)

static const struct device *const cdc_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

/* Handshake state */
static volatile bool handshake_done = false;
static struct k_sem handshake_sem;

/* Sync state */
static struct k_sem sync_sem;

/* DTR semaphore - signaled from USB message callback */
static struct k_sem dtr_sem;
static volatile bool dtr_set = false;

/* Ring buffer for RX data */
#define RX_RING_BUF_SIZE 512
RING_BUF_DECLARE(rx_ring_buf, RX_RING_BUF_SIZE);

/* RX throttle flag */
static bool rx_throttled = false;

/* Receive buffer for signal detection */
#define RX_BUF_SIZE 64
static char rx_buf[RX_BUF_SIZE];
static size_t rx_pos = 0;

/* Signal strings */
static const char *handshake_req = NOLOGO_CDC_HANDSHAKE_REQ;
static const size_t handshake_req_len = sizeof(NOLOGO_CDC_HANDSHAKE_REQ) - 1;

static const char *sync_host_done = NOLOGO_CDC_SYNC_HOST_DONE;
static const size_t sync_host_done_len = sizeof(NOLOGO_CDC_SYNC_HOST_DONE) - 1;

/* Debug counters */
static volatile uint32_t isr_call_count = 0;
static volatile uint32_t rx_byte_count = 0;

/**
 * @brief Check if buffer ends with a given string
 */
static bool check_buffer_ends_with(const char *str, size_t str_len)
{
    if (rx_pos < str_len) {
        return false;
    }

    const char *check_start = rx_buf + rx_pos - str_len;
    return (memcmp(check_start, str, str_len) == 0);
}

/**
 * @brief Check if buffer contains handshake request
 */
static bool check_handshake(void)
{
    return check_buffer_ends_with(handshake_req, handshake_req_len);
}

/**
 * @brief Check if buffer contains sync signal from host
 */
static bool check_sync_host(void)
{
    return check_buffer_ends_with(sync_host_done, sync_host_done_len);
}

/**
 * @brief UART interrupt callback for CDC ACM
 */
static void cdc_uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    isr_call_count++;

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (!rx_throttled && uart_irq_rx_ready(dev)) {
            uint8_t buffer[64];
            size_t space = ring_buf_space_get(&rx_ring_buf);
            size_t len = MIN(space, sizeof(buffer));

            if (len == 0) {
                /* Throttle because ring buffer is full */
                uart_irq_rx_disable(dev);
                rx_throttled = true;
                continue;
            }

            int recv_len = uart_fifo_read(dev, buffer, len);
            if (recv_len > 0) {
                rx_byte_count += recv_len;
                ring_buf_put(&rx_ring_buf, buffer, recv_len);
            }
        }
    }
}

/**
 * @brief Process received data from ring buffer
 */
static void process_rx_data(void)
{
    uint8_t buf[64];
    size_t len;

    while ((len = ring_buf_get(&rx_ring_buf, buf, sizeof(buf))) > 0) {
        /* Re-enable RX if throttled */
        if (rx_throttled) {
            uart_irq_rx_enable(cdc_dev);
            rx_throttled = false;
        }

        for (size_t i = 0; i < len; i++) {
            /* Add to detection buffer (circular) */
            if (rx_pos >= RX_BUF_SIZE - 1) {
                memmove(rx_buf, rx_buf + 1, RX_BUF_SIZE - 1);
                rx_pos = RX_BUF_SIZE - 1;
            }
            rx_buf[rx_pos++] = (char)buf[i];
        }

        /* Check for handshake */
        if (!handshake_done && check_handshake()) {
            printk("cdc: handshake received!\n");

            /* Send response */
            const char *resp = NOLOGO_CDC_HANDSHAKE_RESP;
            size_t resp_len = strlen(resp);
            for (size_t j = 0; j < resp_len; j++) {
                uart_poll_out(cdc_dev, resp[j]);
            }

            handshake_done = true;
            k_sem_give(&handshake_sem);
            nologo_status_blink_green();

            rx_pos = 0;
        }

        /* Check for sync signal from host */
        if (check_sync_host()) {
            printk("cdc: sync signal received from host!\n");
            k_sem_give(&sync_sem);
            nologo_status_blink_green();

            rx_pos = 0;
        }
    }
}

/**
 * @brief CDC receive thread
 */
static void cdc_rx_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (!device_is_ready(cdc_dev)) {
        k_msleep(100);
    }

    printk("cdc: rx thread started\n");

    while (1) {
        /* Wait for DTR signal from USB callback */
        printk("cdc: waiting for DTR...\n");
        k_sem_take(&dtr_sem, K_FOREVER);

        if (!dtr_set) {
            continue;
        }

        printk("cdc: DTR set, host connected!\n");

        /* Set DCD and DSR to signal ready (optional but recommended) */
        uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DCD, 1);
        uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DSR, 1);

        /* Wait 100ms for host to complete all settings (per Zephyr sample) */
        k_msleep(100);

        /* Enable interrupt-driven RX now */
        uart_irq_callback_set(cdc_dev, cdc_uart_isr);
        uart_irq_rx_enable(cdc_dev);

        printk("cdc: RX interrupts enabled, waiting for handshake\n");

        /* Process data while DTR is set */
        while (dtr_set) {
            /* Process any received data */
            if (!ring_buf_is_empty(&rx_ring_buf)) {
                process_rx_data();
            }

            /* Brief sleep to yield CPU */
            k_msleep(10);
        }

        /* DTR cleared, disable RX and reset state */
        uart_irq_rx_disable(cdc_dev);

        printk("cdc: DTR cleared, host disconnected (isr=%u, rx=%u bytes)\n",
               isr_call_count, rx_byte_count);

        /* Reset state for next connection */
        handshake_done = false;
        rx_pos = 0;
        rx_throttled = false;
        ring_buf_reset(&rx_ring_buf);
    }
}

K_THREAD_DEFINE(cdc_rx_thread_id, 2048, cdc_rx_thread,
                NULL, NULL, NULL, 7, 0, K_TICKS_FOREVER);

void nologo_cdc_notify_dtr_set(void)
{
    dtr_set = true;
    k_sem_give(&dtr_sem);
}

void nologo_cdc_notify_dtr_clear(void)
{
    dtr_set = false;
    k_sem_give(&dtr_sem);
}

bool nologo_cdc_is_handshake_done(void)
{
    return handshake_done;
}

int nologo_cdc_wait_handshake(uint32_t timeout_ms)
{
    if (handshake_done) {
        return 0;
    }

    k_timeout_t timeout = (timeout_ms == 0) ? K_FOREVER : K_MSEC(timeout_ms);
    int ret = k_sem_take(&handshake_sem, timeout);

    if (ret == -EAGAIN) {
        return -ETIMEDOUT;
    }

    return 0;
}

int nologo_cdc_wait_host(uint32_t timeout_ms)
{
    k_timeout_t timeout = (timeout_ms == 0) ? K_FOREVER : K_MSEC(timeout_ms);
    int ret = k_sem_take(&sync_sem, timeout);

    if (ret == -EAGAIN) {
        return -ETIMEDOUT;
    }

    return 0;
}

int nologo_cdc_signal_host(void)
{
    if (!device_is_ready(cdc_dev)) {
        return -ENODEV;
    }

    const char *msg = NOLOGO_CDC_SYNC_DEVICE_DONE;
    size_t len = strlen(msg);

    for (size_t i = 0; i < len; i++) {
        uart_poll_out(cdc_dev, msg[i]);
    }

    printk("cdc: sync signal sent to host\n");
    return 0;
}

int nologo_cdc_send(const char *msg, size_t len)
{
    if (!device_is_ready(cdc_dev)) {
        return -ENODEV;
    }

    for (size_t i = 0; i < len; i++) {
        uart_poll_out(cdc_dev, msg[i]);
    }

    return 0;
}

int nologo_cdc_init(void)
{
    if (!device_is_ready(cdc_dev)) {
        return -ENODEV;
    }

    k_sem_init(&handshake_sem, 0, 1);
    k_sem_init(&sync_sem, 0, K_SEM_MAX_LIMIT);
    k_sem_init(&dtr_sem, 0, 1);
    k_thread_start(cdc_rx_thread_id);

    printk("cdc: initialized, waiting for handshake (%s)\n", handshake_req);
    return 0;
}

#else

void nologo_cdc_notify_dtr_set(void) {}
void nologo_cdc_notify_dtr_clear(void) {}

bool nologo_cdc_is_handshake_done(void)
{
    return false;
}

int nologo_cdc_wait_handshake(uint32_t timeout_ms)
{
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
}

int nologo_cdc_wait_host(uint32_t timeout_ms)
{
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
}

int nologo_cdc_signal_host(void)
{
    return -ENOTSUP;
}

int nologo_cdc_send(const char *msg, size_t len)
{
    ARG_UNUSED(msg);
    ARG_UNUSED(len);
    return -ENOTSUP;
}

int nologo_cdc_init(void)
{
    printk("cdc: CDC ACM not enabled\n");
    return -ENOTSUP;
}

#endif
