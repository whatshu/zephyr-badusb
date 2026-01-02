# nologo_usb (RP2350 / Zephyr)

本项目是基于 Zephyr 的 RP2350（`rpi_pico2/rp2350a/m33`）示例工程，目标是提供一个**高效、模块化**的 USB 复合设备固件，并保留 WS2812 状态灯与低开销的系统压力估算。

## 功能列表（对齐需求 1~7）

- **1) printk 路由（Debug/Release 行为不同）**
  - **Debug**：默认将 `printk()` 输出**重定向到 USB CDC**，并且**镜像**到硬件 **UART0**。
  - **Release**：`printk()` **仅输出到 UART0**（不启用 CDC/MSC，也不安装 printk hook）。

- **2) 启动时读取 USB MSC 对应文件系统里的配置文件**
  - 启动阶段从 flash-disk（disk 名 `"NAND"`）对应的 FAT 文件系统读取 **`/NAND:/config`**。
  - 内容读取后保存在静态内存缓冲区中（默认最多 4096 bytes）。

- **3) Debug 默认挂载/暴露 USB MSC**
  - Debug 构建默认启用 USB MSC class，固件启动后主机会看到一个 Mass Storage 盘（flash-backed LUN）。
  - Release 默认关闭 MSC（减少 USB 端点/枚举复杂度，避免不必要的功能暴露）。

- **4) 启用 USB HID（键盘），保留“启动按一次 a”，并保留后续修改能力**
  - HID 接口 ready 后发送一次键盘按键（默认 `HID_KEY_A` press+release）。
  - 预留扩展点：主机通过 HID `SET_REPORT` 发送 1 字节即可更新“启动按键 keycode”（便于后续扩展为更复杂的控制协议）。

- **5) 精简并拆分模块**
  - 移除原先单文件 `main.c` 的大杂烩实现，拆分为多个独立 `.c` 文件，入口文件只保留初始化顺序。

- **6) 保留 LED，并更精确/低开销估算系统压力**
  - 仍然以 20ms tick 为周期，但使用 **cycle 级**时间戳计算调度“迟到时间”（lateness）并做 EWMA。
  - 计算量为常数级，避免高频日志/复杂统计导致额外负载。

- **7) 启用 RP2350 双核**
  - `prj.conf` 中启用 `CONFIG_SMP=y` 和 `CONFIG_MP_MAX_NUM_CPUS=2`，使 Zephyr SMP 使用两个核心（实际可用性取决于当前 Zephyr/板级支持与应用负载）。

## 代码结构

```
zephyr_try/nologo_usb/
  CMakeLists.txt
  prj.conf
  prj.debug.conf
  prj.release.conf
  app.overlay
  src/
    app_main.c
    nologo_build.h
    nologo_usb.c / nologo_usb.h
    nologo_printk_router.c / nologo_printk_router.h
    nologo_hid.c / nologo_hid.h
    nologo_config.c / nologo_config.h
    nologo_status_led.c / nologo_status_led.h
```

### 各模块职责

- **`src/app_main.c`**
  - 只负责**初始化顺序**：状态灯 → printk 路由（Debug）→ 读取 config → HID 注册 → USB 栈启动。

- **`src/nologo_printk_router.c`**
  - Debug 下安装 `__printk_hook_install()`：
    - 将字符写入 USB CDC ring buffer（等 DTR/configured 再 flush）
    - 同时调用 `prev_printk_hook()` 做 UART0 镜像
  - Release 下该模块为 no-op（不改变默认 UART0 console 行为）。

- **`src/nologo_config.c`**
  - 将 `"/NAND:/config"` 文件读入内存缓冲区：
    - 挂载点：`/NAND:`
    - disk：`"NAND"`
    - 默认只读挂载
  - API：
    - `nologo_config_init()`：启动读取
    - `nologo_config_get(&len)`：获取内存中的内容与长度

- **`src/nologo_hid.c`**
  - 注册 HID 键盘（Boot keyboard report desc）。
  - 接口 ready 后发送一次启动按键（默认 `HID_KEY_A`）。
  - `SET_REPORT`：若收到 1 字节，将其作为新的启动 keycode（扩展点）。

- **`src/nologo_usb.c`**
  - 负责 USB device stack（next-gen）初始化、描述符、配置与消息回调：
    - USB configured / CDC DTR：通知 printk router 允许 flush
    - Debug 下若启用 MSC：启动前 `disk_access_init("NAND")`，保证枚举时 LUN 可用

- **`src/nologo_status_led.c`**
  - WS2812 单颗 RGB 状态灯：
    - 绿色：压力脉冲（off 时间随压力变化）
    - 蓝色：printk 活动指示（短时间无输出自动熄灭）
    - 红色：故障锁存
  - 压力估算：以 20ms tick 的周期性事件为基准，计算调度 lateness 并做 EWMA（低开销）。

## 设备树 / 存储布局（`app.overlay`）

- **WS2812**：GPIO22，PIO 驱动，alias 为 `led-strip`
- **flash-backed disk**：`zephyr,flash-disk`，disk-name 为 `"NAND"`
- **MSC 分区**：默认占用 flash0 尾部 512 KiB（示例：`0x00380000 0x00080000`）
- **USB CDC ACM**：在 `&zephyr_udc0` 下定义 `cdc_acm_uart0`
- **HID device**：`zephyr,hid-device`，keyboard 协议

> 注意：若应用体积增长接近 flash 尾部，需要调整 `storage_partition` 的 offset/size。

## 配置文件（Kconfig）

### `prj.conf`（通用）

- UART console / printk / serial
- USB device stack next-gen
- HID support
- flash-disk + FATFS（用于读取 `/NAND:/config`）
- WS2812 LED strip
- SMP 双核：`CONFIG_SMP=y`、`CONFIG_MP_MAX_NUM_CPUS=2`

### `prj.debug.conf`（Debug 额外启用）

- `CONFIG_USBD_CDC_ACM_CLASS=y`
- `CONFIG_USBD_MSC_CLASS=y`

### `prj.release.conf`（Release 默认关闭）

- `CONFIG_USBD_CDC_ACM_CLASS=n`
- `CONFIG_USBD_MSC_CLASS=n`

## Debug/Release 的构建选择逻辑

`CMakeLists.txt` 会根据 `CMAKE_BUILD_TYPE` 自动选择额外的 Kconfig fragment：

- `CMAKE_BUILD_TYPE=Debug` → 使用 `prj.debug.conf`
- 其它（默认 Release） → 使用 `prj.release.conf`

并设置宏：

- Debug：`NOLOGO_DEBUG=1`
- Release：`NOLOGO_DEBUG=0`

## 编译方式

### 使用 west（推荐）

在 `zephyr_try/nologo_usb` 目录下：

#### Debug

> 关键点：CMake 参数要放到 `--` 后面，否则会被 west 当成 source dir。

```bash
west build -p always -b rpi_pico2/rp2350a/m33 -d build_debug . -- -DCMAKE_BUILD_TYPE=Debug
west build -- -DCMAKE_BUILD_TYPE=Debug
```

#### Release

```bash
west build -p always -b rpi_pico2/rp2350a/m33 -d build_release . -- -DCMAKE_BUILD_TYPE=Release
west build
```

产物：

- `build_debug/zephyr/zephyr.uf2`
- `build_release/zephyr/zephyr.uf2`

### 不用 west（直接 CMake）

前提：你已经在正确的 Zephyr 环境里（已设置 `ZEPHYR_BASE`、toolchain 等）。

#### Debug

```bash
cmake -S . -B build_debug -GNinja -DBOARD=rpi_pico2/rp2350a/m33 -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug -j
```

#### Release

```bash
cmake -S . -B build_release -GNinja -DBOARD=rpi_pico2/rp2350a/m33 -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j
```

## 如何确认当前构建是否启用了 Debug 行为

检查 `.config`：

```bash
grep -E "CONFIG_USBD_(CDC_ACM|MSC)_CLASS=" build_debug/zephyr/.config
```

Debug 期望看到：

- `CONFIG_USBD_CDC_ACM_CLASS=y`
- `CONFIG_USBD_MSC_CLASS=y`

Release 期望看到 `=n`。

## 运行时行为总结

- **Debug**
  - `printk`：USB CDC 为主输出，同时镜像到 UART0
  - MSC：主机可看到 mass storage（flash-backed），同时固件启动时会读取 `/NAND:/config`
  - HID：启动发送一次按键（默认 a）

- **Release**
  - `printk`：仅 UART0
  - MSC/CDC：默认关闭
  - HID：保留（仍可作为后续交互入口）

## 可调参数（常用）

- **配置文件路径**：`src/nologo_config.c`
  - 默认 `NOLOGO_CONFIG_PATH="/NAND:/config"`
  - 最大长度 `NOLOGO_CONFIG_MAX_LEN`（默认 4096）
- **HID 启动按键**：`src/nologo_hid.c`
  - 默认 `HID_KEY_A`
  - `SET_REPORT` 1 字节可覆盖 keycode（扩展点）
- **LED 压力显示曲线**：`src/nologo_status_led.c`
  - `pressure_to_pulse_off_ms()` 可按你希望的灵敏度/范围调整


