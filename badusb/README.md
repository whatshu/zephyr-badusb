# nologo_usb / nologo_usb2 (Zephyr)

bad usb application in rust for these two boards.

- **nologo_usb**: Raspberry Pi Pico (RP2040)
- **nologo_usb2**: Raspberry Pi Pico 2 (RP2350A Cortex-M33)

## west env

**the env setup is not part of this project, search for how to install it.**

west toolchain in *../../../zephyr-venv*.

run *source ../../../zephyr-venv/bin/activate* to enable the env.

## function

- USB HID: keyboard
- USB CDC ACM: serial
- USB MSC: storage, only enabled in debug mode, everything is kept in flash
- WS2812: status indicator, GPIO 22
- FAT FS support

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
    nologo_usb.c / nologo_hid.c / nologo_config.c
    nologo_cdc.c / nologo_status_led.c
```
