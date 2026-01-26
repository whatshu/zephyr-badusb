// SPDX-License-Identifier: Apache-2.0
#pragma once

/**
 * @brief Initialize and execute the HID script from config
 *
 * This function parses the config file content using Rubber Ducky-like
 * syntax and executes HID keyboard operations accordingly.
 *
 * Supported commands:
 *   REM <comment>           - Comment line (ignored)
 *   DELAY <ms>              - Delay in milliseconds
 *   STRING <text>           - Type a string
 *   ENTER                   - Press Enter key
 *   TAB                     - Press Tab key
 *   SPACE                   - Press Space key
 *   BACKSPACE               - Press Backspace key
 *   DELETE                  - Press Delete key
 *   ESCAPE / ESC            - Press Escape key
 *   UP / DOWN / LEFT / RIGHT- Arrow keys
 *   HOME / END              - Home/End keys
 *   PAGEUP / PAGEDOWN       - Page Up/Down keys
 *   INSERT                  - Press Insert key
 *   CAPSLOCK                - Press Caps Lock key
 *   NUMLOCK                 - Press Num Lock key
 *   SCROLLLOCK              - Press Scroll Lock key
 *   PRINTSCREEN             - Press Print Screen key
 *   PAUSE                   - Press Pause key
 *   F1 - F12                - Function keys
 *   GUI / WINDOWS <key>     - Windows/Command key + optional key
 *   CTRL <key>              - Control + key
 *   ALT <key>               - Alt + key
 *   SHIFT <key>             - Shift + key
 *   CTRL-ALT <key>          - Control + Alt + key
 *   CTRL-SHIFT <key>        - Control + Shift + key
 *   ALT-SHIFT <key>         - Alt + Shift + key
 *   GUI-SHIFT <key>         - GUI + Shift + key
 *   REPEAT <n>              - Repeat previous command n times
 *   DEFAULT_DELAY <ms>      - Set default delay between commands
 *   STRING_DELAY <ms>       - Set typing speed (delay per key, default 50ms)
 *   WAIT_HANDSHAKE [ms]     - Wait for CDC serial handshake (optional timeout)
 *   WAIT_HOST [ms]          - Wait for host sync signal (optional timeout)
 *   SIGNAL_HOST             - Send sync signal to host
 *
 * @return 0 on success, negative error code on failure
 */
int nologo_script_run(void);

