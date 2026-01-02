# nologo_usb / nologo_usb2 (Zephyr)

基于 Zephyr 的 USB 复合设备固件，支持两个自定义板级：

- **nologo_usb**: Raspberry Pi Pico (RP2040)
- **nologo_usb2**: Raspberry Pi Pico 2 (RP2350A Cortex-M33)

## west 环境

当前目录下的 zephyr 使用的 west 工具环境在 */home/whatshu/develop/project/pico/zephyr-venv* 下.

## 功能

- USB HID 键盘（启动时发送按键）
- USB CDC ACM（始终启用）
- USB MSC 存储（仅 Debug 模式，flash-backed）
- WS2812 状态灯（GPIO22）
- FAT 文件系统配置读取
- printk 输出到 UART0（硬件串口）

## 编译

```bash
# RP2040 Release
west build -p always -b nologo_usb .

# RP2040 Debug
west build -p always -b nologo_usb . -- -DCMAKE_BUILD_TYPE=Debug

# RP2350 Release
west build -p always -b nologo_usb2/rp2350a/m33 .

# RP2350 Debug
west build -p always -b nologo_usb2/rp2350a/m33 . -- -DCMAKE_BUILD_TYPE=Debug
```

产物：`build/zephyr/zephyr.uf2`

## 烧录

将 `.uf2` 拖拽到 Pico 的 USB 存储模式，或使用 OpenOCD/probe-rs/J-Link。

## 代码结构

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
    nologo_printk_router.c / nologo_status_led.c
```
