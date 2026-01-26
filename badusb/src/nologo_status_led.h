// SPDX-License-Identifier: Apache-2.0
#pragma once

/**
 * Initialize the status LED module.
 * Turns on solid RED during initialization phase.
 */
void nologo_status_init(void);

/**
 * Signal that initialization is complete.
 * Turns off the RED LED (unless fault is set).
 */
void nologo_status_init_done(void);

/**
 * Set fault status - RED LED will blink continuously.
 * This overrides normal operation and indicates a critical error.
 */
void nologo_status_set_fault(void);

/**
 * Trigger a brief BLUE LED blink.
 * Call this when HID keyboard output occurs.
 */
void nologo_status_blink_blue(void);

/**
 * Trigger a brief GREEN LED blink.
 * Call this when CDC serial input/output occurs.
 */
void nologo_status_blink_green(void);