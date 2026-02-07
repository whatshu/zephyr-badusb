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

/* String/character input delay (ms) - controls typing speed */
static uint32_t string_delay_ms = 50;

/* Key press/release delay (ms) */
#define KEY_RELEASE_DELAY_MS 10

/* Script execution state */
static volatile bool script_hid_ready;

/* Maximum number of labels */
#define MAX_LABELS 32

/* Maximum loop nesting depth */
#define MAX_LOOP_DEPTH 8

/* Label entry */
struct script_label {
    char name[32];
    size_t line_index;
};

/* Loop stack entry */
struct loop_entry {
    size_t start_line;      /* Line index of LOOP command */
    int remaining;          /* Remaining iterations */
};

/* Script line entry */
struct script_line {
    const char *start;
    size_t len;
};

/* Script execution context */
struct script_context {
    struct script_label labels[MAX_LABELS];
    int label_count;

    struct loop_entry loop_stack[MAX_LOOP_DEPTH];
    int loop_depth;

    struct script_line *lines;
    size_t line_count;
    size_t current_line;

    bool goto_pending;
    size_t goto_target;
};

/* Forward declarations */
static int send_key(uint8_t modifier, uint8_t keycode);
static int send_string(const char *str, size_t len);
static int parse_and_execute_line(const char *line, size_t len, struct script_context *ctx);

/**
 * @brief Find a label by name
 * @return line index or -1 if not found
 */
static int find_label(struct script_context *ctx, const char *name, size_t name_len)
{
    for (int i = 0; i < ctx->label_count; i++) {
        if (strlen(ctx->labels[i].name) == name_len &&
            strncasecmp(ctx->labels[i].name, name, name_len) == 0) {
            return (int)ctx->labels[i].line_index;
        }
    }
    return -1;
}

/**
 * @brief Add a label to the context
 */
static int add_label(struct script_context *ctx, const char *name, size_t name_len, size_t line_index)
{
    if (ctx->label_count >= MAX_LABELS) {
        printk("script: too many labels (max %d)\n", MAX_LABELS);
        return -ENOMEM;
    }

    if (name_len >= sizeof(ctx->labels[0].name)) {
        printk("script: label name too long\n");
        return -EINVAL;
    }

    /* Check for duplicate */
    if (find_label(ctx, name, name_len) >= 0) {
        printk("script: duplicate label [%.*s]\n", (int)name_len, name);
        return -EEXIST;
    }

    memcpy(ctx->labels[ctx->label_count].name, name, name_len);
    ctx->labels[ctx->label_count].name[name_len] = '\0';
    ctx->labels[ctx->label_count].line_index = line_index;
    ctx->label_count++;

    return 0;
}

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

    k_msleep(string_delay_ms);

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
static int parse_and_execute_line(const char *line, size_t len, struct script_context *ctx)
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

    /* LABEL <name> - Define a label (already parsed, just skip) */
    if (strncasecmp(cmd, "LABEL", cmd_len) == 0 && cmd_len == 5) {
        /* Labels are pre-parsed, nothing to do at execution time */
        return 0;
    }

    /* GOTO <name> - Jump to a label */
    if (strncasecmp(cmd, "GOTO", cmd_len) == 0 && cmd_len == 4) {
        size_t label_len;
        const char *label_name = get_token(cmd + cmd_len, end, &label_len);
        if (label_len == 0) {
            printk("script: GOTO missing label name\n");
            return -EINVAL;
        }

        int target = find_label(ctx, label_name, label_len);
        if (target < 0) {
            printk("script: GOTO label not found [%.*s]\n", (int)label_len, label_name);
            return -ENOENT;
        }

        printk("script: GOTO %.*s (line %d)\n", (int)label_len, label_name, target);
        ctx->goto_pending = true;
        ctx->goto_target = (size_t)target;
        return 0;
    }

    /* LOOP <count> - Start a loop */
    if (strncasecmp(cmd, "LOOP", cmd_len) == 0 && cmd_len == 4) {
        const char *arg = skip_whitespace(cmd + cmd_len, end);
        int count = atoi(arg);
        if (count <= 0) {
            printk("script: LOOP invalid count\n");
            return -EINVAL;
        }

        if (ctx->loop_depth >= MAX_LOOP_DEPTH) {
            printk("script: LOOP nesting too deep (max %d)\n", MAX_LOOP_DEPTH);
            return -ENOMEM;
        }

        ctx->loop_stack[ctx->loop_depth].start_line = ctx->current_line;
        ctx->loop_stack[ctx->loop_depth].remaining = count;
        ctx->loop_depth++;

        printk("script: LOOP %d (depth %d)\n", count, ctx->loop_depth);
        return 0;
    }

    /* ENDLOOP - End of loop block */
    if (strncasecmp(cmd, "ENDLOOP", cmd_len) == 0 && cmd_len == 7) {
        if (ctx->loop_depth <= 0) {
            printk("script: ENDLOOP without matching LOOP\n");
            return -EINVAL;
        }

        struct loop_entry *loop = &ctx->loop_stack[ctx->loop_depth - 1];
        loop->remaining--;

        if (loop->remaining > 0) {
            /* Jump back to after the LOOP command */
            printk("script: ENDLOOP - %d iterations remaining\n", loop->remaining);
            ctx->goto_pending = true;
            ctx->goto_target = loop->start_line + 1;
        } else {
            /* Loop finished, pop from stack */
            printk("script: ENDLOOP - loop complete\n");
            ctx->loop_depth--;
        }
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

    /* STRING_DELAY <ms> or STRINGDELAY <ms> - controls typing speed */
    if ((strncasecmp(cmd, "STRING_DELAY", cmd_len) == 0 && cmd_len == 12) ||
        (strncasecmp(cmd, "STRINGDELAY", cmd_len) == 0 && cmd_len == 11)) {
        const char *arg = skip_whitespace(cmd + cmd_len, end);
        string_delay_ms = atoi(arg);
        if (string_delay_ms < 5) {
            string_delay_ms = 5;  /* Minimum 5ms to ensure reliable input */
        }
        printk("script: STRING_DELAY set to %u ms\n", string_delay_ms);
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
 * @brief Pre-parse script to count lines and find labels
 */
static int preparse_script(const uint8_t *script, size_t script_len,
                           struct script_context *ctx)
{
    const char *p = (const char *)script;
    const char *end = p + script_len;
    size_t line_index = 0;

    /* First pass: count lines */
    while (p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }
        line_index++;
        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) {
            p++;
        }
    }

    ctx->line_count = line_index;
    ctx->lines = k_malloc(sizeof(struct script_line) * line_index);
    if (!ctx->lines) {
        printk("script: failed to allocate line array\n");
        return -ENOMEM;
    }

    /* Second pass: store lines and find labels */
    p = (const char *)script;
    line_index = 0;

    while (p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }

        /* Trim trailing whitespace */
        const char *line_trim = line_end;
        while (line_trim > p && (line_trim[-1] == ' ' || line_trim[-1] == '\t')) {
            line_trim--;
        }

        ctx->lines[line_index].start = p;
        ctx->lines[line_index].len = line_trim - p;

        /* Check for LABEL command */
        if (ctx->lines[line_index].len > 6) {
            size_t cmd_len;
            const char *cmd = get_token(p, line_trim, &cmd_len);
            if (cmd_len == 5 && strncasecmp(cmd, "LABEL", 5) == 0) {
                size_t label_len;
                const char *label_name = get_token(cmd + cmd_len, line_trim, &label_len);
                if (label_len > 0) {
                    int ret = add_label(ctx, label_name, label_len, line_index);
                    if (ret != 0 && ret != -EEXIST) {
                        k_free(ctx->lines);
                        ctx->lines = NULL;
                        return ret;
                    }
                    printk("script: found LABEL %.*s at line %zu\n",
                           (int)label_len, label_name, line_index);
                }
            }
        }

        line_index++;
        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) {
            p++;
        }
    }

    return 0;
}

/**
 * @brief Execute the script from config buffer
 */
static int execute_script(const uint8_t *script, size_t script_len)
{
    struct script_context ctx = {0};
    int ret;

    printk("script: executing %u bytes\n", (unsigned)script_len);

    /* Pre-parse script */
    ret = preparse_script(script, script_len, &ctx);
    if (ret != 0) {
        printk("script: preparse failed (%d)\n", ret);
        return ret;
    }

    printk("script: %zu lines, %d labels\n", ctx.line_count, ctx.label_count);

    /* Execute lines */
    ctx.current_line = 0;
    while (ctx.current_line < ctx.line_count) {
        struct script_line *line = &ctx.lines[ctx.current_line];

        if (line->len > 0) {
            ret = parse_and_execute_line(line->start, line->len, &ctx);
            if (ret != 0) {
                printk("script: line %zu failed (%d)\n", ctx.current_line, ret);
            }

            /* Handle goto */
            if (ctx.goto_pending) {
                ctx.goto_pending = false;
                ctx.current_line = ctx.goto_target;
                continue;
            }

            /* Apply default delay after each command */
            if (default_delay_ms > 0) {
                k_msleep(default_delay_ms);
            }
        }

        ctx.current_line++;
    }

    /* Cleanup */
    if (ctx.lines) {
        k_free(ctx.lines);
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
