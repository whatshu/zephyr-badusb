// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Handshake magic string.
 * Client sends this string to initiate handshake.
 * Device responds with HANDSHAKE_RESP.
 */
#define NOLOGO_CDC_HANDSHAKE_REQ  "NOLOGO_SHAKE"
#define NOLOGO_CDC_HANDSHAKE_RESP "NOLOGO_ACK\r\n"

/**
 * Synchronization signals.
 * Used for bidirectional sync between device and host.
 *
 * Host -> Device: Host sends SYNC_HOST_DONE to signal completion
 * Device -> Host: Device sends SYNC_DEVICE_DONE to signal completion
 */
#define NOLOGO_CDC_SYNC_HOST_DONE   "NOLOGO_SYNC"
#define NOLOGO_CDC_SYNC_DEVICE_DONE "NOLOGO_DONE\r\n"

/**
 * Initialize the CDC serial interface.
 */
int nologo_cdc_init(void);

/**
 * Check if handshake has been completed.
 * @return true if handshake completed, false otherwise
 */
bool nologo_cdc_is_handshake_done(void);

/**
 * Wait for handshake to complete with timeout.
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int nologo_cdc_wait_handshake(uint32_t timeout_ms);

/**
 * Wait for host to send sync signal (NOLOGO_SYNC).
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int nologo_cdc_wait_host(uint32_t timeout_ms);

/**
 * Signal host that device has completed (sends NOLOGO_DONE).
 * @return 0 on success, negative error code on failure
 */
int nologo_cdc_signal_host(void);

/**
 * Send a message via CDC serial.
 * @param msg Message to send
 * @param len Length of message
 * @return 0 on success, negative error code on failure
 */
int nologo_cdc_send(const char *msg, size_t len);

/**
 * Notify CDC module that DTR has been set (host connected).
 * Called from USB message callback.
 */
void nologo_cdc_notify_dtr_set(void);

/**
 * Notify CDC module that DTR has been cleared (host disconnected).
 * Called from USB message callback.
 */
void nologo_cdc_notify_dtr_clear(void);
