// SPDX-License-Identifier: Apache-2.0

#include "nologo_script.h"
#include "nologo_cdc.h"
#include "nologo_config.h"
#include "nologo_status_led.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <zephyr/drivers/gpio.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* HID device */
static const struct device *const hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);

/* Script disable pin: GPIO2
 * When HIGH, script execution is disabled (safety feature)
 */
#define SCRIPT_DISABLE_PIN 2
static const struct device *const gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* Keyboard report buffer */
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

UDC_STATIC_BUF_DEFINE(script_kbd_report, KB_REPORT_SIZE);

/* Default delay between commands (ms) */
static uint32_t default_delay_ms = 0;

/* Key press/release delay (ms) */
#define KEY_PRESS_DELAY_MS   50
#define KEY_RELEASE_DELAY_MS 20

/* Script execution state */
static volatile bool script_hid_ready;

/* Forward declarations */
static int send_key(uint8_t modifier, uint8_t keycode);
static int send_string(const char *str, size_t len);
static int parse_and_execute_line(const char *line, size_t len);

/**
 * @brief Character to HID keycode mapping
 */
struct char_keycode {
    uint8_t keycode;
    bool shift;
};

/* US keyboard layout mapping */
static struct char_keycode char_to_keycode(char c)
{
    struct char_keycode result = {0, false};

    if (c >= 'a' && c <= 'z') {
        result.keycode = HID_KEY_A + (c - 'a');
        result.shift = false;
    } else if (c >= 'A' && c <= 'Z') {
        result.keycode = HID_KEY_A + (c - 'A');
        result.shift = true;
    } else if (c >= '1' && c <= '9') {
        result.keycode = HID_KEY_1 + (c - '1');
        result.shift = false;
    } else if (c == '0') {
        result.keycode = HID_KEY_0;
        result.shift = false;
    } else {
        switch (c) {
        case ' ':  result.keycode = HID_KEY_SPACE; result.shift = false; break;
        case '\n': result.keycode = HID_KEY_ENTER; result.shift = false; break;
        case '\t': result.keycode = HID_KEY_TAB; result.shift = false; break;
        case '-':  result.keycode = HID_KEY_MINUS; result.shift = false; break;
        case '_':  result.keycode = HID_KEY_MINUS; result.shift = true; break;
        case '=':  result.keycode = HID_KEY_EQUAL; result.shift = false; break;
        case '+':  result.keycode = HID_KEY_EQUAL; result.shift = true; break;
        case '[':  result.keycode = HID_KEY_LEFTBRACE; result.shift = false; break;
        case '{':  result.keycode = HID_KEY_LEFTBRACE; result.shift = true; break;
        case ']':  result.keycode = HID_KEY_RIGHTBRACE; result.shift = false; break;
        case '}':  result.keycode = HID_KEY_RIGHTBRACE; result.shift = true; break;
        case '\\': result.keycode = HID_KEY_BACKSLASH; result.shift = false; break;
        case '|':  result.keycode = HID_KEY_BACKSLASH; result.shift = true; break;
        case ';':  result.keycode = HID_KEY_SEMICOLON; result.shift = false; break;
        case ':':  result.keycode = HID_KEY_SEMICOLON; result.shift = true; break;
        case '\'': result.keycode = HID_KEY_APOSTROPHE; result.shift = false; break;
        case '"':  result.keycode = HID_KEY_APOSTROPHE; result.shift = true; break;
        case '`':  result.keycode = HID_KEY_GRAVE; result.shift = false; break;
        case '~':  result.keycode = HID_KEY_GRAVE; result.shift = true; break;
        case ',':  result.keycode = HID_KEY_COMMA; result.shift = false; break;
        case '<':  result.keycode = HID_KEY_COMMA; result.shift = true; break;
        case '.':  result.keycode = HID_KEY_DOT; result.shift = false; break;
        case '>':  result.keycode = HID_KEY_DOT; result.shift = true; break;
        case '/':  result.keycode = HID_KEY_SLASH; result.shift = false; break;
        case '?':  result.keycode = HID_KEY_SLASH; result.shift = true; break;
        case '!':  result.keycode = HID_KEY_1; result.shift = true; break;
        case '@':  result.keycode = HID_KEY_2; result.shift = true; break;
        case '#':  result.keycode = HID_KEY_3; result.shift = true; break;
        case '$':  result.keycode = HID_KEY_4; result.shift = true; break;
        case '%':  result.keycode = HID_KEY_5; result.shift = true; break;
        case '^':  result.keycode = HID_KEY_6; result.shift = true; break;
        case '&':  result.keycode = HID_KEY_7; result.shift = true; break;
        case '*':  result.keycode = HID_KEY_8; result.shift = true; break;
        case '(':  result.keycode = HID_KEY_9; result.shift = true; break;
        case ')':  result.keycode = HID_KEY_0; result.shift = true; break;
        default:   result.keycode = 0; break;
        }
    }

    return result;
}

/**
 * @brief Send a key press and release
 */
static int send_key(uint8_t modifier, uint8_t keycode)
{
    int ret;

    /* Key press */
    memset(script_kbd_report, 0, KB_REPORT_SIZE);
    script_kbd_report[KB_MOD_KEY] = modifier;
    script_kbd_report[KB_KEY_CODE1] = keycode;

    ret = hid_device_submit_report(hid_dev, KB_REPORT_SIZE, script_kbd_report);
    if (ret != 0) {
        printk("script: key press failed (%d)\n", ret);
        return ret;
    }

    k_msleep(KEY_PRESS_DELAY_MS);

    /* Key release */
    memset(script_kbd_report, 0, KB_REPORT_SIZE);
    ret = hid_device_submit_report(hid_dev, KB_REPORT_SIZE, script_kbd_report);
    if (ret != 0) {
        printk("script: key release failed (%d)\n", ret);
        return ret;
    }

    k_msleep(KEY_RELEASE_DELAY_MS);

    return 0;
}

/**
 * @brief Send a string as keyboard input
 */
static int send_string(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        struct char_keycode ck = char_to_keycode(str[i]);

        if (ck.keycode == 0) {
            /* Skip unsupported characters */
            continue;
        }

        uint8_t modifier = ck.shift ? HID_KBD_MODIFIER_LEFT_SHIFT : 0;
        int ret = send_key(modifier, ck.keycode);
        if (ret != 0) {
            return ret;
        }

        nologo_status_blink_blue();
    }

    return 0;
}

/**
 * @brief Parse a key name and return its keycode
 */
static uint8_t parse_keyname(const char *name, size_t len)
{
    if (len == 0) {
        return 0;
    }

    /* Single character key */
    if (len == 1) {
        struct char_keycode ck = char_to_keycode(name[0]);
        return ck.keycode;
    }

    /* Named keys */
    if (strncasecmp(name, "ENTER", len) == 0 && len == 5) return HID_KEY_ENTER;
    if (strncasecmp(name, "RETURN", len) == 0 && len == 6) return HID_KEY_ENTER;
    if (strncasecmp(name, "TAB", len) == 0 && len == 3) return HID_KEY_TAB;
    if (strncasecmp(name, "SPACE", len) == 0 && len == 5) return HID_KEY_SPACE;
    if (strncasecmp(name, "BACKSPACE", len) == 0 && len == 9) return HID_KEY_BACKSPACE;
    if (strncasecmp(name, "DELETE", len) == 0 && len == 6) return HID_KEY_DELETE;
    if (strncasecmp(name, "DEL", len) == 0 && len == 3) return HID_KEY_DELETE;
    if (strncasecmp(name, "ESCAPE", len) == 0 && len == 6) return HID_KEY_ESC;
    if (strncasecmp(name, "ESC", len) == 0 && len == 3) return HID_KEY_ESC;
    if (strncasecmp(name, "INSERT", len) == 0 && len == 6) return HID_KEY_INSERT;
    if (strncasecmp(name, "HOME", len) == 0 && len == 4) return HID_KEY_HOME;
    if (strncasecmp(name, "END", len) == 0 && len == 3) return HID_KEY_END;
    if (strncasecmp(name, "PAGEUP", len) == 0 && len == 6) return HID_KEY_PAGEUP;
    if (strncasecmp(name, "PAGEDOWN", len) == 0 && len == 8) return HID_KEY_PAGEDOWN;
    if (strncasecmp(name, "UP", len) == 0 && len == 2) return HID_KEY_UP;
    if (strncasecmp(name, "UPARROW", len) == 0 && len == 7) return HID_KEY_UP;
    if (strncasecmp(name, "DOWN", len) == 0 && len == 4) return HID_KEY_DOWN;
    if (strncasecmp(name, "DOWNARROW", len) == 0 && len == 9) return HID_KEY_DOWN;
    if (strncasecmp(name, "LEFT", len) == 0 && len == 4) return HID_KEY_LEFT;
    if (strncasecmp(name, "LEFTARROW", len) == 0 && len == 9) return HID_KEY_LEFT;
    if (strncasecmp(name, "RIGHT", len) == 0 && len == 5) return HID_KEY_RIGHT;
    if (strncasecmp(name, "RIGHTARROW", len) == 0 && len == 10) return HID_KEY_RIGHT;
    if (strncasecmp(name, "CAPSLOCK", len) == 0 && len == 8) return HID_KEY_CAPSLOCK;
    if (strncasecmp(name, "NUMLOCK", len) == 0 && len == 7) return HID_KEY_NUMLOCK;
    if (strncasecmp(name, "SCROLLLOCK", len) == 0 && len == 10) return HID_KEY_SCROLLLOCK;
    if (strncasecmp(name, "PRINTSCREEN", len) == 0 && len == 11) return HID_KEY_SYSRQ;
    if (strncasecmp(name, "PAUSE", len) == 0 && len == 5) return HID_KEY_PAUSE;
    if (strncasecmp(name, "BREAK", len) == 0 && len == 5) return HID_KEY_PAUSE;

    /* Function keys */
    if (len >= 2 && len <= 3 && (name[0] == 'F' || name[0] == 'f')) {
        int fnum = atoi(name + 1);
        if (fnum >= 1 && fnum <= 12) {
            return HID_KEY_F1 + (fnum - 1);
        }
    }

    /* Letter keys */
    if (len == 1 && isalpha((unsigned char)name[0])) {
        char c = toupper((unsigned char)name[0]);
        return HID_KEY_A + (c - 'A');
    }

    return 0;
}

/**
 * @brief Skip whitespace and return pointer to next token
 */
static const char *skip_whitespace(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

/**
 * @brief Get next token (word) from line
 */
static const char *get_token(const char *p, const char *end, size_t *len)
{
    const char *start = skip_whitespace(p, end);
    const char *tok_end = start;

    while (tok_end < end && *tok_end != ' ' && *tok_end != '\t' &&
           *tok_end != '\n' && *tok_end != '\r') {
        tok_end++;
    }

    *len = tok_end - start;
    return start;
}

/**
 * @brief Parse and execute a single line of script
 */
static int parse_and_execute_line(const char *line, size_t len)
{
    if (len == 0) {
        return 0;
    }

    const char *end = line + len;
    size_t cmd_len;
    const char *cmd = get_token(line, end, &cmd_len);

    if (cmd_len == 0) {
        return 0;
    }

    /* REM - Comment */
    if (strncasecmp(cmd, "REM", cmd_len) == 0 && cmd_len == 3) {
        return 0;
    }

    /* DELAY <ms> */
    if (strncasecmp(cmd, "DELAY", cmd_len) == 0 && cmd_len == 5) {
        const char *arg = skip_whitespace(cmd + cmd_len, end);
        int delay_ms = atoi(arg);
        if (delay_ms > 0) {
            printk("script: DELAY %d ms\n", delay_ms);
            k_msleep(delay_ms);
        }
        return 0;
    }

    /* DEFAULT_DELAY <ms> or DEFAULTDELAY <ms> */
    if ((strncasecmp(cmd, "DEFAULT_DELAY", cmd_len) == 0 && cmd_len == 13) ||
        (strncasecmp(cmd, "DEFAULTDELAY", cmd_len) == 0 && cmd_len == 12)) {
        const char *arg = skip_whitespace(cmd + cmd_len, end);
        default_delay_ms = atoi(arg);
        printk("script: DEFAULT_DELAY set to %u ms\n", default_delay_ms);
        return 0;
    }

    /* WAIT_HANDSHAKE [timeout_ms] - Wait for CDC serial handshake */
    if ((strncasecmp(cmd, "WAIT_HANDSHAKE", cmd_len) == 0 && cmd_len == 14) ||
        (strncasecmp(cmd, "WAITHANDSHAKE", cmd_len) == 0 && cmd_len == 13)) {
        const char *arg = skip_whitespace(cmd + cmd_len, end);
        uint32_t timeout_ms = 0;
        if (arg < end && *arg != '\0') {
            timeout_ms = (uint32_t)atoi(arg);
        }
        printk("script: WAIT_HANDSHAKE (timeout=%u ms)\n", timeout_ms);
        int ret = nologo_cdc_wait_handshake(timeout_ms);
        if (ret == 0) {
            printk("script: handshake completed!\n");
            nologo_status_blink_green();
        } else {
            printk("script: handshake timeout\n");
        }
        return ret;
    }

    /* WAIT_HOST [timeout_ms] - Wait for host to send sync signal */
    if ((strncasecmp(cmd, "WAIT_HOST", cmd_len) == 0 && cmd_len == 9) ||
        (strncasecmp(cmd, "WAITHOST", cmd_len) == 0 && cmd_len == 8)) {
        const char *arg = skip_whitespace(cmd + cmd_len, end);
        uint32_t timeout_ms = 0;
        if (arg < end && *arg != '\0') {
            timeout_ms = (uint32_t)atoi(arg);
        }
        printk("script: WAIT_HOST (timeout=%u ms)\n", timeout_ms);
        int ret = nologo_cdc_wait_host(timeout_ms);
        if (ret == 0) {
            printk("script: host sync received!\n");
            nologo_status_blink_green();
        } else {
            printk("script: wait host timeout\n");
        }
        return ret;
    }

    /* SIGNAL_HOST - Signal host that device has completed */
    if ((strncasecmp(cmd, "SIGNAL_HOST", cmd_len) == 0 && cmd_len == 11) ||
        (strncasecmp(cmd, "SIGNALHOST", cmd_len) == 0 && cmd_len == 10)) {
        printk("script: SIGNAL_HOST\n");
        int ret = nologo_cdc_signal_host();
        if (ret == 0) {
            nologo_status_blink_green();
        }
        return ret;
    }

    /* STRING <text> */
    if (strncasecmp(cmd, "STRING", cmd_len) == 0 && cmd_len == 6) {
        const char *text = cmd + cmd_len;
        /* Skip single space after STRING */
        if (text < end && *text == ' ') {
            text++;
        }
        size_t text_len = end - text;
        printk("script: STRING [%.*s]\n", (int)text_len, text);
        return send_string(text, text_len);
    }

    /* STRINGLN <text> - STRING with ENTER at end */
    if (strncasecmp(cmd, "STRINGLN", cmd_len) == 0 && cmd_len == 8) {
        const char *text = cmd + cmd_len;
        if (text < end && *text == ' ') {
            text++;
        }
        size_t text_len = end - text;
        printk("script: STRINGLN [%.*s]\n", (int)text_len, text);
        int ret = send_string(text, text_len);
        if (ret == 0) {
            ret = send_key(0, HID_KEY_ENTER);
        }
        return ret;
    }

    /* Modifier key combinations */
    uint8_t modifier = 0;
    const char *key_arg = NULL;
    size_t key_arg_len = 0;

    /* GUI / WINDOWS */
    if ((strncasecmp(cmd, "GUI", cmd_len) == 0 && cmd_len == 3) ||
        (strncasecmp(cmd, "WINDOWS", cmd_len) == 0 && cmd_len == 7) ||
        (strncasecmp(cmd, "COMMAND", cmd_len) == 0 && cmd_len == 7) ||
        (strncasecmp(cmd, "META", cmd_len) == 0 && cmd_len == 4)) {
        modifier = HID_KBD_MODIFIER_LEFT_UI;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* CTRL / CONTROL */
    if ((strncasecmp(cmd, "CTRL", cmd_len) == 0 && cmd_len == 4) ||
        (strncasecmp(cmd, "CONTROL", cmd_len) == 0 && cmd_len == 7)) {
        modifier = HID_KBD_MODIFIER_LEFT_CTRL;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* ALT */
    if (strncasecmp(cmd, "ALT", cmd_len) == 0 && cmd_len == 3) {
        modifier = HID_KBD_MODIFIER_LEFT_ALT;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* SHIFT */
    if (strncasecmp(cmd, "SHIFT", cmd_len) == 0 && cmd_len == 5) {
        modifier = HID_KBD_MODIFIER_LEFT_SHIFT;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* CTRL-ALT */
    if ((strncasecmp(cmd, "CTRL-ALT", cmd_len) == 0 && cmd_len == 8) ||
        (strncasecmp(cmd, "CTRL_ALT", cmd_len) == 0 && cmd_len == 8)) {
        modifier = HID_KBD_MODIFIER_LEFT_CTRL | HID_KBD_MODIFIER_LEFT_ALT;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* CTRL-SHIFT */
    if ((strncasecmp(cmd, "CTRL-SHIFT", cmd_len) == 0 && cmd_len == 10) ||
        (strncasecmp(cmd, "CTRL_SHIFT", cmd_len) == 0 && cmd_len == 10)) {
        modifier = HID_KBD_MODIFIER_LEFT_CTRL | HID_KBD_MODIFIER_LEFT_SHIFT;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* ALT-SHIFT */
    if ((strncasecmp(cmd, "ALT-SHIFT", cmd_len) == 0 && cmd_len == 9) ||
        (strncasecmp(cmd, "ALT_SHIFT", cmd_len) == 0 && cmd_len == 9)) {
        modifier = HID_KBD_MODIFIER_LEFT_ALT | HID_KBD_MODIFIER_LEFT_SHIFT;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* GUI-SHIFT */
    if ((strncasecmp(cmd, "GUI-SHIFT", cmd_len) == 0 && cmd_len == 9) ||
        (strncasecmp(cmd, "GUI_SHIFT", cmd_len) == 0 && cmd_len == 9)) {
        modifier = HID_KBD_MODIFIER_LEFT_UI | HID_KBD_MODIFIER_LEFT_SHIFT;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* CTRL-GUI */
    if ((strncasecmp(cmd, "CTRL-GUI", cmd_len) == 0 && cmd_len == 8) ||
        (strncasecmp(cmd, "CTRL_GUI", cmd_len) == 0 && cmd_len == 8)) {
        modifier = HID_KBD_MODIFIER_LEFT_CTRL | HID_KBD_MODIFIER_LEFT_UI;
        key_arg = get_token(cmd + cmd_len, end, &key_arg_len);
        goto process_modifier_key;
    }

    /* Single key commands */
    if (strncasecmp(cmd, "ENTER", cmd_len) == 0 && cmd_len == 5) {
        printk("script: ENTER\n");
        return send_key(0, HID_KEY_ENTER);
    }
    if (strncasecmp(cmd, "RETURN", cmd_len) == 0 && cmd_len == 6) {
        printk("script: RETURN\n");
        return send_key(0, HID_KEY_ENTER);
    }
    if (strncasecmp(cmd, "TAB", cmd_len) == 0 && cmd_len == 3) {
        printk("script: TAB\n");
        return send_key(0, HID_KEY_TAB);
    }
    if (strncasecmp(cmd, "SPACE", cmd_len) == 0 && cmd_len == 5) {
        printk("script: SPACE\n");
        return send_key(0, HID_KEY_SPACE);
    }
    if (strncasecmp(cmd, "BACKSPACE", cmd_len) == 0 && cmd_len == 9) {
        printk("script: BACKSPACE\n");
        return send_key(0, HID_KEY_BACKSPACE);
    }
    if ((strncasecmp(cmd, "DELETE", cmd_len) == 0 && cmd_len == 6) ||
        (strncasecmp(cmd, "DEL", cmd_len) == 0 && cmd_len == 3)) {
        printk("script: DELETE\n");
        return send_key(0, HID_KEY_DELETE);
    }
    if ((strncasecmp(cmd, "ESCAPE", cmd_len) == 0 && cmd_len == 6) ||
        (strncasecmp(cmd, "ESC", cmd_len) == 0 && cmd_len == 3)) {
        printk("script: ESCAPE\n");
        return send_key(0, HID_KEY_ESC);
    }
    if (strncasecmp(cmd, "INSERT", cmd_len) == 0 && cmd_len == 6) {
        printk("script: INSERT\n");
        return send_key(0, HID_KEY_INSERT);
    }
    if (strncasecmp(cmd, "HOME", cmd_len) == 0 && cmd_len == 4) {
        printk("script: HOME\n");
        return send_key(0, HID_KEY_HOME);
    }
    if (strncasecmp(cmd, "END", cmd_len) == 0 && cmd_len == 3) {
        printk("script: END\n");
        return send_key(0, HID_KEY_END);
    }
    if (strncasecmp(cmd, "PAGEUP", cmd_len) == 0 && cmd_len == 6) {
        printk("script: PAGEUP\n");
        return send_key(0, HID_KEY_PAGEUP);
    }
    if (strncasecmp(cmd, "PAGEDOWN", cmd_len) == 0 && cmd_len == 8) {
        printk("script: PAGEDOWN\n");
        return send_key(0, HID_KEY_PAGEDOWN);
    }
    if ((strncasecmp(cmd, "UP", cmd_len) == 0 && cmd_len == 2) ||
        (strncasecmp(cmd, "UPARROW", cmd_len) == 0 && cmd_len == 7)) {
        printk("script: UP\n");
        return send_key(0, HID_KEY_UP);
    }
    if ((strncasecmp(cmd, "DOWN", cmd_len) == 0 && cmd_len == 4) ||
        (strncasecmp(cmd, "DOWNARROW", cmd_len) == 0 && cmd_len == 9)) {
        printk("script: DOWN\n");
        return send_key(0, HID_KEY_DOWN);
    }
    if ((strncasecmp(cmd, "LEFT", cmd_len) == 0 && cmd_len == 4) ||
        (strncasecmp(cmd, "LEFTARROW", cmd_len) == 0 && cmd_len == 9)) {
        printk("script: LEFT\n");
        return send_key(0, HID_KEY_LEFT);
    }
    if ((strncasecmp(cmd, "RIGHT", cmd_len) == 0 && cmd_len == 5) ||
        (strncasecmp(cmd, "RIGHTARROW", cmd_len) == 0 && cmd_len == 10)) {
        printk("script: RIGHT\n");
        return send_key(0, HID_KEY_RIGHT);
    }
    if (strncasecmp(cmd, "CAPSLOCK", cmd_len) == 0 && cmd_len == 8) {
        printk("script: CAPSLOCK\n");
        return send_key(0, HID_KEY_CAPSLOCK);
    }
    if (strncasecmp(cmd, "NUMLOCK", cmd_len) == 0 && cmd_len == 7) {
        printk("script: NUMLOCK\n");
        return send_key(0, HID_KEY_NUMLOCK);
    }
    if (strncasecmp(cmd, "SCROLLLOCK", cmd_len) == 0 && cmd_len == 10) {
        printk("script: SCROLLLOCK\n");
        return send_key(0, HID_KEY_SCROLLLOCK);
    }
    if (strncasecmp(cmd, "PRINTSCREEN", cmd_len) == 0 && cmd_len == 11) {
        printk("script: PRINTSCREEN\n");
        return send_key(0, HID_KEY_SYSRQ);
    }
    if (strncasecmp(cmd, "PAUSE", cmd_len) == 0 && cmd_len == 5) {
        printk("script: PAUSE\n");
        return send_key(0, HID_KEY_PAUSE);
    }
    if (strncasecmp(cmd, "BREAK", cmd_len) == 0 && cmd_len == 5) {
        printk("script: BREAK\n");
        return send_key(0, HID_KEY_PAUSE);
    }

    /* Function keys F1-F12 */
    if (cmd_len >= 2 && cmd_len <= 3 && (cmd[0] == 'F' || cmd[0] == 'f')) {
        int fnum = atoi(cmd + 1);
        if (fnum >= 1 && fnum <= 12) {
            printk("script: F%d\n", fnum);
            return send_key(0, HID_KEY_F1 + (fnum - 1));
        }
    }

    /* MENU (Application key) */
    if (strncasecmp(cmd, "MENU", cmd_len) == 0 && cmd_len == 4) {
        printk("script: MENU\n");
        return send_key(0, 0x65); /* Application key */
    }

    /* APP (same as MENU) */
    if (strncasecmp(cmd, "APP", cmd_len) == 0 && cmd_len == 3) {
        printk("script: APP\n");
        return send_key(0, 0x65);
    }

    printk("script: unknown command [%.*s]\n", (int)cmd_len, cmd);
    return 0;

process_modifier_key:
    if (key_arg_len > 0) {
        uint8_t keycode = parse_keyname(key_arg, key_arg_len);
        if (keycode != 0) {
            printk("script: modifier 0x%02X + key 0x%02X\n", modifier, keycode);
            nologo_status_blink_blue();
            return send_key(modifier, keycode);
        }
    }
    /* Modifier key alone */
    printk("script: modifier 0x%02X alone\n", modifier);
    nologo_status_blink_blue();
    return send_key(modifier, 0);
}

/**
 * @brief Execute the script from config buffer
 */
static int execute_script(const uint8_t *script, size_t script_len)
{
    const char *p = (const char *)script;
    const char *end = p + script_len;

    printk("script: executing %u bytes\n", (unsigned)script_len);

    while (p < end) {
        /* Find end of line */
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }

        /* Trim trailing whitespace */
        const char *line_trim = line_end;
        while (line_trim > p && (line_trim[-1] == ' ' || line_trim[-1] == '\t')) {
            line_trim--;
        }

        /* Execute line if not empty */
        size_t line_len = line_trim - p;
        if (line_len > 0) {
            int ret = parse_and_execute_line(p, line_len);
            if (ret != 0) {
                printk("script: line failed (%d)\n", ret);
            }

            /* Apply default delay after each command */
            if (default_delay_ms > 0) {
                k_msleep(default_delay_ms);
            }
        }

        /* Skip line ending */
        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) {
            p++;
        }
    }

    printk("script: execution complete\n");
    return 0;
}

/**
 * @brief HID interface ready callback
 */
static void script_hid_iface_ready(const struct device *dev, const bool ready)
{
    ARG_UNUSED(dev);
    script_hid_ready = ready;
    printk("script: HID iface %s\n", ready ? "ready" : "not ready");
}

static int script_hid_get_report(const struct device *dev, const uint8_t type, const uint8_t id,
                                  const uint16_t len, uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
    return 0;
}

static int script_hid_set_report(const struct device *dev, const uint8_t type, const uint8_t id,
                                  const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
    return 0;
}

static void script_hid_set_idle(const struct device *dev, const uint8_t id, const uint32_t duration)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    ARG_UNUSED(duration);
}

static uint32_t script_hid_get_idle(const struct device *dev, const uint8_t id)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    return 0;
}

static void script_hid_set_protocol(const struct device *dev, const uint8_t proto)
{
    ARG_UNUSED(dev);
    printk("script: protocol %s\n", proto == 0U ? "boot" : "report");
}

static void script_hid_output_report(const struct device *dev, const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);
}

static const struct hid_device_ops script_hid_ops = {
    .iface_ready = script_hid_iface_ready,
    .get_report = script_hid_get_report,
    .set_report = script_hid_set_report,
    .set_idle = script_hid_set_idle,
    .get_idle = script_hid_get_idle,
    .set_protocol = script_hid_set_protocol,
    .output_report = script_hid_output_report,
};

static const uint8_t script_hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

/**
 * @brief Check if script execution is disabled via GPIO2
 * @return true if disabled (GPIO2 is HIGH), false if enabled
 */
static bool script_is_disabled(void)
{
    if (!device_is_ready(gpio_dev)) {
        printk("script: GPIO device not ready, assuming enabled\n");
        return false;
    }

    int ret = gpio_pin_configure(gpio_dev, SCRIPT_DISABLE_PIN,
                                  GPIO_INPUT | GPIO_PULL_DOWN);
    if (ret != 0) {
        printk("script: failed to configure GPIO%d (%d)\n", SCRIPT_DISABLE_PIN, ret);
        return false;
    }

    int value = gpio_pin_get(gpio_dev, SCRIPT_DISABLE_PIN);
    if (value < 0) {
        printk("script: failed to read GPIO%d (%d)\n", SCRIPT_DISABLE_PIN, value);
        return false;
    }

    return (value == 1);
}

/**
 * @brief Script execution thread
 */
static void script_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    /* Wait for HID interface to be ready */
    while (!script_hid_ready) {
        k_msleep(50);
    }

    /* Check if script execution is disabled via GPIO2 */
    if (script_is_disabled()) {
        printk("script: DISABLED (GPIO%d is HIGH)\n", SCRIPT_DISABLE_PIN);
        nologo_status_blink_red();
        k_sleep(K_FOREVER);
        return;
    }

    /* Wait 1 second after device is ready before executing script */
    printk("script: waiting 1 second before execution\n");
    k_msleep(1000);

    /* Get config and execute */
    size_t config_len;
    const uint8_t *config = nologo_config_get(&config_len);

    if (config_len > 0) {
        execute_script(config, config_len);
        nologo_status_blink_green();
    } else {
        printk("script: no config to execute\n");
    }

    k_sleep(K_FOREVER);
}

K_THREAD_DEFINE(script_thread_id, 2048, script_thread,
                NULL, NULL, NULL, 7, 0, K_TICKS_FOREVER);

int nologo_script_run(void)
{
    if (!device_is_ready(hid_dev)) {
        printk("script: HID device not ready\n");
        nologo_status_set_fault();
        return -ENODEV;
    }

    int ret = hid_device_register(hid_dev, script_hid_report_desc,
                                   sizeof(script_hid_report_desc), &script_hid_ops);
    if (ret != 0) {
        printk("script: HID register failed (%d)\n", ret);
        nologo_status_set_fault();
        return ret;
    }

    k_thread_start(script_thread_id);
    printk("script: initialized\n");
    return 0;
}
