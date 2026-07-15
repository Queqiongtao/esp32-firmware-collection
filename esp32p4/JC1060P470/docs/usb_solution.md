# USB 子系统方案

## 概述

本工程 USB 子系统围绕 ESP32-P4 的两个 USB Type-C 口构建。两个口能力不同，不能互换：

- `FULL USB`（丝印）= USB Serial/JTAG（USJ）固定功能 CDC-ACM，只用于烧录和 IDF 日志。
- `HIGH USB`（丝印）= 内部 UTMI High-Speed PHY + DWC OTG controller[0]，480Mbps，可作 HS Device 或 HS Host，**同一时刻只能选一种角色**。

USB 应用层目前由两个独立测试模块构成：

- `test_usb`：TinyUSB HS Device CDC-ACM echo（已实现，已上板验证）。
- `test_uvc`：usb_host + esp-usb UVC Host + 硬件 JPEG 解码 + PPA 缩放预览（已实现）。

`test_usb_role` 角色管理模块让 `HIGH USB` 在运行时于 CDC Device 与 UVC Host 之间切换，并由 LVGL 按钮触发。本文档记录端口划分、资源模型、运行时角色切换、UVC 格式协商与恢复策略。

> **验证状态声明**：当前改动已完成 ESP-IDF `v6.0.1` 编译验证。热复位自动恢复、PPA 预览性能和重复角色切换仍需上板验证，不应仅凭编译结果表述为“稳定”。

## UVC 热复位恢复与 VBUS 边界

JC1060P470 原理图中 `HIGH USB` 的 VBUS 供电路径没有连接到 ESP32-P4 GPIO 的独立 load-switch enable。`usb_host_lib_set_root_port_power(false)` 会停止 DWC root port、断开下游设备并阻止重新枚举，但不等价于切断摄像头 5V。

当前软件恢复流程为：

1. USB Host 以 `root_port_unpowered=true` 安装，UVC client 和事件任务就绪后再开启 root port。
2. 成功出流后在 RTC no-init 内存记录恢复标记；正常退出 UVC 时清除，热复位中断出流时由 USB role worker 自动恢复 UVC 角色。
3. 连续 5 秒没有视频帧时，由 USB role worker 串行触发恢复，禁止在 UVC callback 或 frame worker 内直接卸载自身。
4. 恢复依次尝试重新打开 stream、root port 逻辑关闭/开启、完整卸载并重建 UVC Host/DWC PHY/JPEG/PPA；每级有界重试并退避。
5. UVC 卸载期间保持 USB Host event task 运行，直到收到 `NO_CLIENTS` 且设备已释放或 `ALL_FREE`，再卸载 Host。
6. 恢复耗尽后停止自动重试，UI 显示错误码并提示重新插拔。若摄像头固件必须真实掉电才能复位，只能增加 GPIO 控制的 USB load switch 或使用支持端口供电控制的 Hub。

## USB 端口与控制器总览

集中定义于 `main/test_config.h` “USB ports overview” 注释段：

| 丝印 | 板级连接 | 控制器 | 能力 |
|------|----------|--------|------|
| `FULL USB` | GPIO `24(DM)/25(DP)` | `USB_INT_PHY0` = USJ | 固定功能 CDC-ACM 串口，固件下载 / IDF log。**不是 DWC OTG**，不能跑 TinyUSB 自定义 device 栈或 USB Host。 |
| `HIGH USB` | 封装脚 `49(DM)/50(DP)` | 内部 UTMI HS PHY → DWC OTG controller[0] | 480Mbps。可作 HS Device（TinyUSB CDC/MSC/HID）或 HS Host（usb_host + UVC/MSC/HID），同一时刻只能选一种角色。 |

要点：

- ESP32-P4 V1.0 上 `USB_INT_PHY0`（GPIO24/25）路由到 USJ 控制器，不是 DWC OTG。`USB_INT_PHY1`（GPIO26/27）虽可路由到 DWC OTG controller[1] 做 FS OTG，但 GPIO27 与 `DISPLAY_PIN_NUM_LCD_RST` 冲突，本工程不使用该路径。
- `HIGH USB` 的 UTMI PHY 是封装专用脚，不进 GPIO 矩阵，不会触碰显示/触摸/SDMMC 引脚。
- USJ 与 HS OTG 在主机侧都可能枚举为串口设备，但功能不同：USJ 仅烧录/日志，HS OTG 是应用层 CDC/Host 数据口。

## controller[0] 资源模型

`HIGH USB` 的 DWC OTG controller[0] + UTMI PHY 是被 `test_usb` 与 `test_uvc` 共享的硬件资源。ESP-IDF `v6.0.1` 通过 `esp_private/usb_phy.h` 的 PHY 抽象层管理其占用与角色：

| API（`components/esp_hw_support/usb_phy/usb_phy.c`） | 行为 |
|------|------|
| `usb_new_phy(config, &handle)` | 申请 PHY handle。UTMI PHY 用引用计数 + 占用互斥；若已被占用返回 `ESP_ERR_INVALID_STATE`（源码第 279 行）。`config->otg_mode` 在第 325 行强制调用 `usb_phy_otg_set_mode` 设置当前角色。 |
| `usb_phy_otg_set_mode(handle, HOST/DEVICE)` | 切换 OTG 角色信号矩阵（iddig/bvalid/vbusvalid/avalid）与 D+/D- 上下拉。**不改 PHY 占用状态**，仅切 GPIO matrix（源码第 136-191 行）。 |
| `usb_del_phy(handle)` | 释放 PHY handle，减引用计数；ref_count 归零时禁用 USB peripheral（源码第 364-384 行）。 |

依据：

- TinyUSB device（`esp_tinyusb`）在 `tinyusb_driver_install` 时调用 `usb_new_phy(target=UTMI, controller=OTG, otg_mode=DEVICE)` 占用 UTMI PHY；`tinyusb_driver_uninstall` 时调 `usb_del_phy` 释放。
- usb_host（`usb_host_install` 通过 `skip_phy_setup=false`）调用 `usb_new_phy(target=UTMI, controller=OTG, otg_mode=HOST)` 占用 UTMI PHY；`usb_host_uninstall` 时释放。
- 由于 UTMI PHY 占用互斥（`ESP_ERR_INVALID_STATE`），两个 stack 不可能同时成功 install。这是“同一时刻只能选一种角色”的底层保障。

> **静态分析结论**：运行时切换在底层 API 层是支持的——先 uninstall 一方释放 PHY handle，再 install 另一方重新申请。但 esp_tinyusb / usb_host 的 uninstall 是否完整重置 DWC OTG core（中断使能、FIFO 配置、endpoint 状态、session 状态）属于 stack 内部行为，应用层不可控，需要一次上板验证才能定性。

## 当前 USB Demo 现状

### 模块清单

| 模块 | 文件 | 角色 | 状态 |
|------|------|------|------|
| CDC Device | `main/test_usb.c/.h` | TinyUSB HS Device CDC-ACM echo | 已实现，已上板验证（CDC echo 路径） |
| UVC Host | `main/test_uvc.c/.h` | usb_host + UVC + JPEG + PPA 预览与分级恢复 | 已实现，待本轮上板验证 |
| 角色管理 | `main/test_usb_role.c/.h` | 统一协调 controller[0] 切换与恢复 | 已实现，待本轮上板验证 |

### test_usb：CDC Device echo

- TinyUSB HS Device，CDC-ACM 端口 0（`TINYUSB_CDC_ACM_0`）。
- RX 异步：CDC RX 回调把消息投递到 event queue，worker task `usb_cdc_evt` 调 `tinyusb_cdcacm_read` 读出数据，hex dump + 原样回写。
- 上报 line state（DTR/RTS）。
- `test_usb_get_status` 提供 capability 风格只读状态（attached/dtr/rts/rx_packets/tx_packets/rx_bytes/tx_bytes/last_error）。
- `test_usb_deinit` 完整 uninstall（cdcacm deinit → tinyusb_driver_uninstall → 删 task/queue）。

### test_uvc：UVC Host + 硬件 JPEG 解码

- usb_host 通过 `peripheral_map = BIT0` 选 controller[0]（HS OTG + UTMI PHY）。
- esp-usb `uvc_host` driver 后台 task 处理 USB Host 事件；`uvc_frame_callback` 把帧指针投递到 frame queue，worker task `uvc_worker` 用 ESP32-P4 硬件 JPEG 解码器把 MJPEG 解码为 RGB565。
- 阶段 A：只做日志观测（帧序号/字节/宽高/解码耗时/错误码），不写 LVGL 画布。
- 帧缓冲与 RGB565 输出缓冲均分配在 PSRAM（`MALLOC_CAP_SPIRAM`）。
- `test_uvc_get_status` 提供 capability 风格只读状态（initialized/stream_opened/stream_started/device_connected/frames_received/frames_decoded/decode_errors/last_frame_*/last_error）。
- `test_uvc_deinit` 完整 uninstall（stream stop/close → uvc_host_uninstall → usb_host_lib_unblock → 等 usblib task 退出 → usb_host_uninstall → jpeg_del_decoder_engine → 删 task/queue/buf）。

### 阶段 A 的 cache 一致性约束

依据 `test_config.h` “USB UVC camera” 注释段：

- UVC 帧缓冲在 PSRAM，由 USB-DWC DMA 写入，应用 CPU 读 `frame->data`。esp-usb UVC 驱动内部已处理该 DMA→CPU 方向的 cache invalidate，应用层无需额外 `esp_cache_msync`。
- JPEG 硬解输入 `frame->data`（PSRAM，CPU 不写，硬件 DMA 读）无需 msync；输出 RGB565 缓冲在 PSRAM 由 JPEG DMA 写入、CPU 读，无 DMA2D 缩放时不存在 CPU 写后 DMA 读的中间断点，阶段 A 暂不涉及 msync。
- 阶段 B 引入 DMA2D 缩放上屏时，再在 CPU 写 PSRAM 后交 DMA2D 前补 `esp_cache_msync(DIR_C2M)`，遵循 AGENTS.md cache 一致性约束。

## 运行时角色切换方案（规划中）

### 目标

让 `HIGH USB` 在运行时于 CDC Device 与 UVC Host 之间切换，由 LVGL 按钮触发，不再依赖编译期 Kconfig 二选一固化。

### 可行性依据（静态分析）

- `usb_phy_otg_set_mode` 提供角色切换 API（`esp_private/usb_phy.h`）。
- `usb_new_phy` / `usb_del_phy` 引用计数 + 占用互斥，保证两个 stack 不同时 install。
- 现有 `test_usb_deinit` 与 `test_uvc_deinit` 已经在各自路径中调用了对应的 uninstall（`tinyusb_driver_uninstall`、`usb_host_uninstall`、`uvc_host_uninstall`、`jpeg_del_decoder_engine`），证明 stack 的 uninstall API 存在且被设计为可释放。
- 风险：esp_tinyusb / usb_host 的 uninstall 是否完整重置 DWC OTG core，属 stack 内部行为，无法纯静态证明，需一次上板验证。

### test_usb_role 框架 API（规划）

```c
typedef enum {
    TEST_USB_ROLE_NONE = 0,
    TEST_USB_ROLE_CDC_DEVICE,   // test_usb (TinyUSB HS Device CDC)
    TEST_USB_ROLE_UVC_HOST,     // test_uvc (USB Host UVC + JPEG)
} test_usb_role_t;

typedef struct {
    test_usb_role_t current_role;
    bool cdc_attached;
    bool uvc_device_connected;
    bool uvc_stream_started;
    esp_err_t last_switch_error;
} test_usb_role_status_t;

esp_err_t test_usb_role_init(void);                          // 根据 Kconfig 选初始角色
esp_err_t test_usb_role_switch(test_usb_role_t target);     // 异步：投递请求给 role worker task
test_usb_role_t test_usb_role_current(void);
esp_err_t test_usb_role_get_status(test_usb_role_status_t *out);
```

### 切换流程

切换在独立 `role_worker` task 里执行，避免阻塞 LVGL 任务（deinit/init 可能耗时秒级）：

1. `current == target` 直接返回。
2. `current != NONE` → 调对应 `test_usb_deinit()` 或 `test_uvc_deinit()`；两者内部已实现 task 等待退出与资源释放。
3. 调目标 `test_xxx_init() + test_xxx_run()`。
4. 失败回滚到 `TEST_USB_ROLE_NONE` 并设 `last_switch_error`。

### UI 触发（规划）

`lvgl_demo_ui.c` 加一张 “USB Role” 卡片：

- 显示当前角色（CDC / UVC / None）+ 切换状态（Idle / Switching... / OK / Failed）。
- 一个按钮，文案根据当前角色切换（“Switch to UVC” / “Switch to CDC”）。
- 按钮点击 → `test_usb_role_switch(target)` 异步排队。
- 状态通过 `test_usb_role_get_status` 轮询显示，UI 不直接操作底层硬件句柄（遵守 AGENTS.md 解耦约束）。

## UVC 格式协商方案（规划中）

### 当前问题

`test_config.h` 硬编码 `TEST_UVC_H_RES=1280`、`TEST_UVC_V_RES=720`、`TEST_UVC_FPS=30`，`test_uvc_run()` 把这三个值直接传给 `uvc_host_stream_open`。若摄像头描述符不支持该组合，`uvc_host_stream_open` 会协商失败。Kconfig UVC 选项帮助文字残留 “1280x720 MJPEG at 15 FPS”、`test_config.h` 注释残留 “15fps 上限”，与 `TEST_UVC_FPS=30` 不一致。

### 协商原则

- 不硬编码 FPS/分辨率，先读设备描述符，再按偏好匹配。
- `test_config.h` 把 `TEST_UVC_H_RES/V_RES/FPS` 改为 `TEST_UVC_PREFER_H_RES/V_RES`，仅作为“优先目标”而非强制要求。
- 在 `uvc_driver_event_callback` 的 `UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED` 回调里读取 `event->device_connected` 上报的 frame_info 列表（esp-usb UVC 驱动在该事件里已经枚举好设备能力），按优先级匹配：
  1. 优先 MJPEG 格式；
  2. 在 MJPEG 格式下优先匹配 `TEST_UVC_PREFER_H_RES × TEST_UVC_PREFER_V_RES`；
  3. 若偏好分辨率不支持，降级到摄像头实际支持的最大 MJPEG 分辨率；
  4. 在选定分辨率下取摄像头支持的最高 fps。
- `test_uvc_run()` 用动态选定的 format 调 `uvc_host_stream_open`，日志打印实际协商结果（分辨率/fps/format）。

### 待确认项

- esp-usb UVC 驱动的 `device_connected` 事件 payload 字段名/类型需在实施时读 `usb_host_uvc/include/usb/uvc_host.h` 确认（当前 `managed_components/` 未下载，无法静态确认）。
- 若驱动未提供“先看设备能力再 open”的钩子，则降级方案：`uvc_host_stream_open` 失败时打印实际支持的 format/frame 列表，提示用户改 `test_config.h` 的偏好值。

## Kconfig 互斥与启动初始角色

当前 `main/Kconfig.projbuild` 的两个开关：

| 选项 | 默认 | 含义 |
|------|------|------|
| `EXAMPLE_ENABLE_TEST_USB` | `n`（规划改为 `y`） | 启用 TinyUSB CDC-ACM Device echo |
| `EXAMPLE_ENABLE_TEST_UVC` | `y`（规划改为 `n`） | 启用 USB UVC Host + JPEG 解码 |

### 互斥强制（规划）

两个开关共享 controller[0]，必须互斥。规划加双向 `depends on`：

```
config EXAMPLE_ENABLE_TEST_USB
    depends on !EXAMPLE_ENABLE_TEST_UVC
config EXAMPLE_ENABLE_TEST_UVC
    depends on !EXAMPLE_ENABLE_TEST_USB
```

或 main.c 加编译期断言：

```c
_Static_assert(!(CONFIG_EXAMPLE_ENABLE_TEST_USB && CONFIG_EXAMPLE_ENABLE_TEST_UVC),
              "USB CDC and UVC share controller[0], enable one or the other");
```

### 启动初始角色（规划）

引入 `test_usb_role` 后，Kconfig 选项语义改为“启动时初始角色”，运行时仍可切换：

- `EXAMPLE_ENABLE_TEST_USB=y` + `EXAMPLE_ENABLE_TEST_UVC=n` → 启动进入 `TEST_USB_ROLE_CDC_DEVICE`。
- `EXAMPLE_ENABLE_TEST_USB=n` + `EXAMPLE_ENABLE_TEST_UVC=y` → 启动进入 `TEST_USB_ROLE_UVC_HOST`。
- main.c 不再直接调 `test_usb_init/test_uvc_init`，改调 `test_usb_role_init()`。
- 默认值规划改为 CDC on / UVC off（USB Device 优先）。

## 与 AGENTS.md 约束对应

| AGENTS.md 约束 | USB 子系统落地 |
|------|------|
| 不重复配置同一硬件资源 | controller[0] UTMI PHY 由 esp_tinyusb / usb_host 各自 install 时申请、uninstall 时释放，应用层不绕过 stack 直接操作 PHY handle。 |
| 模块对外接口收敛为 capability | `test_usb` / `test_uvc` / `test_usb_role` 均暴露 `init/run/deinit/get_status`，不直穿其他模块内部状态。 |
| 硬件测试逻辑与 UI 展示逻辑解耦 | `test_usb_role_switch` 是 capability 接口，LVGL 按钮只调它，不直接操作底层 USB 句柄。 |
| 资源生命周期先定义再写逻辑 | 切换流程严格 deinit 当前 → init 目标，task/queue/handle 生命周期由各模块 deinit 内部管理。 |
| Cache 一致性 | 阶段 A 不涉及 CPU 写后 DMA 读的中间断点；阶段 B 上屏时由 display 模块提供 capability 接口统一调度 DMA2D + `esp_cache_msync(DIR_C2M)`，UVC 模块不直拿 framebuffer 句柄。 |
| 不做与当前目标无关的重构 | test_usb_role 模块新增属“与目标相关的新功能”，不顺手改 test_usb/test_uvc 既有路径。 |
| 对“已验证”表述必须克制 | 运行时切换与 UVC 设备能力枚举均标注为“规划中/静态分析”，不写成“可用/稳定”。 |

## 验证状态

| 路径 | 状态 | 说明 |
|------|------|------|
| CDC Device echo（test_usb 单路径） | 已上板验证 | CDC 枚举、RX hex dump、回显、line state 上报正常 |
| UVC Host 收流 + JPEG 硬解（test_uvc 单路径，阶段 A） | 已上板验证 | 帧序号/字节/宽高/解码耗时/错误码日志正常，未上屏 |
| 运行时角色切换（CDC ↔ UVC） | 已实现，待验证 | 需上板循环验证 uninstall 是否完整释放 DWC OTG |
| UVC 设备能力枚举 + 偏好降级 | 已实现，待验证 | 从设备 frame descriptor 选择 MJPEG 分辨率与最高 FPS |
| UVC 热复位分级恢复 | 已实现，待验证 | 软件不能保证真实 VBUS 断电，恢复耗尽后提示重新插拔 |
| LVGL UVC 预览 | 已实现，待验证 | PPA RGB565 缩放，CPU 回退，默认 10 FPS |
| Kconfig 互斥强制 + 默认值调整 | 规划中，未实现 | 当前两个开关无 `depends on` 约束，可同时设 `y` 导致冲突 |

## 后续工作项

以下方向均属独立工作项，应单独提交验证：

1. **Phase 1：必改项修复**
   - 注释修正：`test_uvc.h` 与 `JC1060P470_main.c` 中 “Full-Speed” 误标为 “High-Speed”。
   - Kconfig 互斥强制 + 默认值调整（CDC on / UVC off）。
   - FPS/分辨率从硬编码改为设备能力枚举（依赖 Phase 2 的 frame_info 读取）。

2. **Phase 2：test_usb_role 模块**
   - 新增 `main/test_usb_role.c/.h`，实现 role worker task 异步切换。
   - main.c 改调 `test_usb_role_init()`。
   - 一次上板验证：CDC ↔ UVC 切换是否稳定，uninstall 是否完整释放 DWC OTG。

3. **Phase 3：LVGL USB Role 卡片**
   - `lvgl_demo_ui.c` 加卡片与按钮，通过 `test_usb_role_switch` 触发，`test_usb_role_get_status` 轮询显示。

4. **UVC 阶段 B（远期）**
   - RGB565 经 DMA2D 缩放上 LVGL canvas，由 display 模块提供 capability 接口统一调度 DMA2D + cache msync，UVC 模块不直拿 framebuffer 句柄。
