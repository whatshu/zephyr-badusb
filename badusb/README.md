# nologo_usb / nologo_usb2 (Zephyr)

本项目是基于 Zephyr 的 USB 复合设备固件，支持两个自定义板级：

- **nologo_usb**: 基于 Raspberry Pi Pico (RP2040)
- **nologo_usb2**: 基于 Raspberry Pi Pico 2 (RP2350A Cortex-M33)

## 功能列表

- **1) printk 路由（Debug/Release 行为不同）**
  - **Debug**：默认将 `printk()` 输出**重定向到 USB CDC**，并且**镜像**到硬件 **UART0**。
  - **Release**：`printk()` **仅输出到 UART0**（不启用 CDC/MSC，也不安装 printk hook）。

- **2) 启动时读取 USB MSC 对应文件系统里的配置文件**
  - 启动阶段从 flash-disk（disk 名 `"NAND"`）对应的 FAT 文件系统读取 **`/NAND:/config`**。
  - 内容读取后保存在静态内存缓冲区中（默认最多 4096 bytes）。

- **3) Debug 默认挂载/暴露 USB MSC**
  - Debug 构建默认启用 USB MSC class，固件启动后主机会看到一个 Mass Storage 盘（flash-backed LUN）。
  - Release 默认关闭 MSC（减少 USB 端点/枚举复杂度，避免不必要的功能暴露）。

- **4) 启用 USB HID（键盘），保留"启动按一次 a"，并保留后续修改能力**
  - HID 接口 ready 后发送一次键盘按键（默认 `HID_KEY_A` press+release）。
  - 预留扩展点：主机通过 HID `SET_REPORT` 发送 1 字节即可更新"启动按键 keycode"。

- **5) WS2812 状态灯**
  - GPIO22 上的 WS2812 RGB LED，用于状态指示。
  - 绿色：系统压力脉冲
  - 蓝色：printk 活动
  - 红色：故障锁存

## 自定义板级描述

项目包含两个自定义板级定义，位于 `boards/nologo/` 目录：

### nologo_usb (RP2040)

- 基于 Raspberry Pi Pico (RP2040)
- 双核 Cortex-M0+ @ 125MHz
- 4 MiB Flash (512 KiB 保留给 USB MSC 存储)
- WS2812 LED on GPIO22
- USB HID + CDC + MSC

### nologo_usb2 (RP2350)

- 基于 Raspberry Pi Pico 2 (RP2350A)
- 双核 Cortex-M33 @ 150MHz
- 4 MiB Flash (512 KiB 保留给 USB MSC 存储)
- WS2812 LED on GPIO22
- USB HID + CDC + MSC

## 代码结构

```
badusb/
  CMakeLists.txt
  prj.conf
  prj.debug.conf
  prj.release.conf
  prj.smp.conf          # SMP 配置参考（暂不可用）
  boards/
    nologo/
      nologo_usb/       # RP2040 板级定义
        board.yml
        board.cmake
        Kconfig*
        nologo_usb.dts
        nologo_usb-pinctrl.dtsi
        nologo_usb_defconfig
        nologo_usb.yaml
      nologo_usb2/      # RP2350 板级定义
        board.yml
        board.cmake
        Kconfig*
        nologo_usb2_rp2350a_m33.dts
        nologo_usb2-pinctrl.dtsi
        nologo_usb2_rp2350a_m33_defconfig
        nologo_usb2_rp2350a_m33.yaml
  src/
    app_main.c
    nologo_build.h
    nologo_usb.c / nologo_usb.h
    nologo_printk_router.c / nologo_printk_router.h
    nologo_hid.c / nologo_hid.h
    nologo_config.c / nologo_config.h
    nologo_status_led.c / nologo_status_led.h
```

## 编译方式

### 使用 west（推荐）

在 `badusb/badusb` 目录下：

#### nologo_usb (RP2040) - Debug

```bash
west build -p always -b nologo_usb -d build_debug . -- -DCMAKE_BUILD_TYPE=Debug
```

#### nologo_usb (RP2040) - Release

```bash
west build -p always -b nologo_usb -d build_release .
```

#### nologo_usb2 (RP2350) - Debug

```bash
west build -p always -b nologo_usb2/rp2350a/m33 -d build_debug . -- -DCMAKE_BUILD_TYPE=Debug
```

#### nologo_usb2 (RP2350) - Release

```bash
west build -p always -b nologo_usb2/rp2350a/m33 -d build_release .
```

产物位于 `build_*/zephyr/zephyr.uf2`。

### 不用 west（直接 CMake）

需要先设置 Zephyr 环境变量：

```bash
source /path/to/zephyr-env.sh
```

#### nologo_usb (RP2040)

```bash
cmake -S . -B build -GNinja -DBOARD=nologo_usb
cmake --build build -j
```

#### nologo_usb2 (RP2350)

```bash
cmake -S . -B build -GNinja -DBOARD=nologo_usb2/rp2350a/m33
cmake --build build -j
```

## SMP / 多核支持说明

### RP2040 (Cortex-M0+)

**Zephyr 不支持 RP2040 的 SMP。**

原因：
- Cortex-M0+ 没有 LDREX/STREX 原子指令
- Zephyr SMP 需要这些指令来实现自旋锁
- 无法在纯软件层面模拟所需的原子操作

替代方案：
- 使用 Pico SDK 的 `pico_multicore` 库进行手动多核控制
- 在 Zephyr 层面使用单核 + 多线程协作调度

### RP2350 (Cortex-M33)

**Zephyr SMP 支持正在开发中。**

RP2350 的 Cortex-M33 具有完整的 ARMv8-M 原子指令支持，理论上可以支持 SMP。
但截至目前，Zephyr 上游尚未完全实现 RP2350 的 SMP 支持。

当上游支持可用时，可通过以下配置启用：

```
CONFIG_SMP=y
CONFIG_MP_MAX_NUM_CPUS=2
```

### 当前推荐做法

使用 Zephyr 的协作式多线程模型：
- 创建多个线程，使用优先级调度
- 使用 `k_work` / `k_work_delayable` 工作队列
- 使用消息队列、FIFO、信号量等内核原语

这在单核上可以提供有效的并发处理能力。

## 配置文件

### prj.conf（通用）

基础 USB 设备栈、HID、文件系统、WS2812 LED 配置。

### prj.debug.conf（Debug 额外启用）

```
CONFIG_USBD_CDC_ACM_CLASS=y
CONFIG_USBD_MSC_CLASS=y
```

### prj.release.conf（Release 默认关闭）

```
CONFIG_USBD_CDC_ACM_CLASS=n
CONFIG_USBD_MSC_CLASS=n
```

## 烧录

将生成的 `.uf2` 文件拖拽到 Pico/Pico2 的 USB 存储模式下即可。

或使用 OpenOCD/probe-rs/J-Link 等调试器。

## 验证构建配置

```bash
grep -E "CONFIG_USBD_(CDC_ACM|MSC)_CLASS=" build/zephyr/.config
```

- Debug: 应该看到 `=y`
- Release: 应该看到 `=n`
