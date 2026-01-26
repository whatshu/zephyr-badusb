# nologo_usb / nologo_usb2 (Zephyr)

bad usb application in rust for these two boards.

- **nologo_usb**: Raspberry Pi Pico (RP2040)
- **nologo_usb2**: Raspberry Pi Pico 2 (RP2350A Cortex-M33)

## west env

**the env setup is not part of this project, search for how to install it.**

west toolchain in *../../../zephyr-venv*.

run *source ../../../zephyr-venv/bin/activate* to enable the env.

## function

- USB HID: keyboard with Rubber Ducky script support.
- USB CDC ACM: serial for dual comm with handshake support.
- USB MSC: storage, only enabled in debug mode. but everything is kept in flash in release mode.
- WS2812: status indicator, GPIO 22. green for usb serial, red for init and error, blue for hid key stroke.
- GPIO2: script disable pin. Pull HIGH to disable script execution (safety feature).
- FAT FS support.

## Rubber Ducky Script Syntax

The device reads a config file from `/NAND:/config` and executes HID keyboard commands using Rubber Ducky-like syntax. The script executes **1 second after device boot**.

### Supported Commands

| Command | Description |
|---------|-------------|
| `REM <comment>` | Comment line (ignored) |
| `DELAY <ms>` | Delay in milliseconds |
| `DEFAULT_DELAY <ms>` | Set default delay between all commands |
| `STRING <text>` | Type a text string |
| `STRINGLN <text>` | Type text and press Enter |
| `WAIT_HANDSHAKE [ms]` | Wait for CDC serial handshake (optional timeout) |
| `WAIT_HOST [ms]` | Wait for host sync signal (optional timeout) |
| `SIGNAL_HOST` | Send sync signal to host |

### Single Key Commands

| Command | Key |
|---------|-----|
| `ENTER` / `RETURN` | Enter key |
| `TAB` | Tab key |
| `SPACE` | Space key |
| `BACKSPACE` | Backspace key |
| `DELETE` / `DEL` | Delete key |
| `ESCAPE` / `ESC` | Escape key |
| `INSERT` | Insert key |
| `HOME` | Home key |
| `END` | End key |
| `PAGEUP` | Page Up key |
| `PAGEDOWN` | Page Down key |
| `UP` / `UPARROW` | Up arrow |
| `DOWN` / `DOWNARROW` | Down arrow |
| `LEFT` / `LEFTARROW` | Left arrow |
| `RIGHT` / `RIGHTARROW` | Right arrow |
| `CAPSLOCK` | Caps Lock |
| `NUMLOCK` | Num Lock |
| `SCROLLLOCK` | Scroll Lock |
| `PRINTSCREEN` | Print Screen |
| `PAUSE` / `BREAK` | Pause/Break key |
| `F1` - `F12` | Function keys |
| `MENU` / `APP` | Application/Context menu key |

### Modifier Key Combinations

| Command | Description |
|---------|-------------|
| `GUI <key>` / `WINDOWS <key>` | Windows/Command key + optional key |
| `CTRL <key>` / `CONTROL <key>` | Control + key |
| `ALT <key>` | Alt + key |
| `SHIFT <key>` | Shift + key |
| `CTRL-ALT <key>` | Control + Alt + key |
| `CTRL-SHIFT <key>` | Control + Shift + key |
| `ALT-SHIFT <key>` | Alt + Shift + key |
| `GUI-SHIFT <key>` | GUI + Shift + key |
| `CTRL-GUI <key>` | Control + GUI + key |

### Example Script

```
REM Open Notepad and type Hello World
DEFAULT_DELAY 50

DELAY 500
GUI r
DELAY 500
STRING notepad
ENTER
DELAY 1000
STRING Hello, World!
ENTER
```

### CDC Serial Handshake & Synchronization

The device supports handshake and bidirectional synchronization via CDC serial port.

**Handshake Protocol (one-time authorization):**

| Direction | Signal | Description |
|-----------|--------|-------------|
| Host → Device | `NOLOGO_SHAKE` | Initiate handshake |
| Device → Host | `NOLOGO_ACK\r\n` | Handshake acknowledged |

**Sync Protocol (bidirectional, repeatable):**

| Direction | Signal | Description |
|-----------|--------|-------------|
| Host → Device | `NOLOGO_SYNC` | Host signals completion |
| Device → Host | `NOLOGO_DONE\r\n` | Device signals completion |

**Usage in script:**

```
REM Wait for initial handshake
WAIT_HANDSHAKE

REM Do some HID operations...
STRING Step 1 complete
ENTER

REM Signal host that step is done
SIGNAL_HOST

REM Wait for host to complete its operation
WAIT_HOST

REM Continue with next step
STRING Step 2 starting...
```

**Workflow Example:**

1. Device boots, script executes `WAIT_HANDSHAKE`
2. Host sends `NOLOGO_SHAKE`, device responds `NOLOGO_ACK`
3. Device executes HID commands, then `SIGNAL_HOST` sends `NOLOGO_DONE`
4. Host processes the result, then sends `NOLOGO_SYNC`
5. Device's `WAIT_HOST` unblocks, script continues

**Running the Sync Demo:**

1. First, run the host sync script on Windows:

```powershell
# Auto-detect CDC port (recommended)
.\examples\sync_host.ps1

# Or specify port manually
.\examples\sync_host.ps1 -ComPort COM3
```

2. Then plug in the BadUSB device (with `config_handshake.txt` as config)

3. The sync script will:
   - Auto-detect the CDC port
   - Send handshake (`NOLOGO_SHAKE`)
   - Wait for device's `NOLOGO_DONE` signal
   - Send sync signal (`NOLOGO_SYNC`)

4. The device will:
   - Wait for handshake before executing HID commands
   - Open Notepad after handshake
   - Signal host when ready
   - Wait for host sync before completing

### Example Config Files

See the `examples/` directory for more sample scripts:

- `config_hello_world.txt` - Basic Notepad example
- `config_handshake.txt` - CDC serial handshake & sync example
- `sync_host.ps1` - PowerShell host script for sync demo
- `config_open_terminal.txt` - Open PowerShell/Terminal
- `config_linux_terminal.txt` - Linux terminal commands
- `config_macos_spotlight.txt` - macOS Spotlight usage
- `config_keyboard_test.txt` - Test all key types
- `config_rickroll.txt` - Fun example
- `config_wifi_password.txt` - Extract WiFi passwords (Windows)
- `config_disable_defender.txt` - Disable Windows Defender
- `config_reverse_shell.txt` - PowerShell reverse shell

## compile

```bash
# RP2040 Release
west build -p always -b nologo_usb .

# RP2040 Debug
west build -p always -b nologo_usb . -- -DCMAKE_BUILD_TYPE=Debug

# RP2350 Release
west build -p always -b nologo_usb2 .

# RP2350 Debug
west build -p always -b nologo_usb2 . -- -DCMAKE_BUILD_TYPE=Debug
```

output: `build/zephyr/zephyr.uf2`

## flash

uf2 flash.

## project structure

```
badusb/
  CMakeLists.txt
  prj.conf / prj.debug.conf / prj.release.conf
  boards/nologo/
    nologo_usb/       # RP2040
    nologo_usb2/      # RP2350
  src/
    app_main.c
    nologo_usb.c / nologo_hid.c / nologo_script.c
    nologo_config.c / nologo_cdc.c / nologo_status_led.c
  examples/
    config_*.txt      # Example Rubber Ducky scripts
```
