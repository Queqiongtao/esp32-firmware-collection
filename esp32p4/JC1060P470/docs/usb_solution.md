# USB 子系统方案

## 概述

本工程有两个能力不同、不能互换的 USB Type-C 口：

- `FULL USB`（丝印）走 GPIO24/25 对应的 USB Serial/JTAG（USJ），固定提供 CDC-ACM 烧录与日志串口，不是 DWC OTG。
- `HIGH USB`（丝印）走封装专用脚 49/50 和内部 UTMI High-Speed PHY，对应 DWC OTG controller[0]，可作为 TinyUSB HS Device 或 USB HS Host，但同一时刻只能有一个角色占用。

应用层由三个模块组成：

| 模块 | 职责 | 当前状态 |
|------|------|----------|
| `test_usb` | TinyUSB HS Device CDC-ACM echo | 已实现；CDC 单路径有上板验证记录 |
| `test_uvc` | UVC Host、MJPEG 能力协商、硬件 JPEG 解码、RGB565 预览和恢复 | 已实现；收流与 JPEG 单路径有上板记录，最新预览/恢复待验证 |
| `test_usb_role` | 串行管理 controller[0] 角色、分辨率切换与健康恢复 | 已实现；重复角色切换和热复位恢复待验证 |

当前代码已完成 ESP-IDF v6.0.1 编译验证。除表格中明确注明的历史记录外，不把新路径表述为已经稳定上板验证。

## 端口与资源模型

| 丝印 | 板级连接 | 控制器与能力 |
|------|----------|--------------|
| `FULL USB` | GPIO24(DM)/GPIO25(DP) | `USB_INT_PHY0` 对应 USJ 固定设备，只用于烧录和日志 |
| `HIGH USB` | 封装脚 49(DM)/50(DP) | UTMI HS PHY + DWC OTG controller[0]，480 Mbps，可作 HS Device 或 HS Host |

ESP32-P4 V1.0 的 GPIO26/27 可路由到 controller[1] 的 FS OTG，但 GPIO27 已用于 LCD reset，本工程不使用该路径。HIGH USB 的 UTMI 信号不进入 GPIO matrix，不与显示、触摸或 SDMMC GPIO 冲突。

TinyUSB Device 和 USB Host 都通过 IDF USB PHY 层申请同一个 UTMI PHY：

- `tinyusb_driver_install()` 以 Device 模式申请，`tinyusb_driver_uninstall()` 释放。
- `usb_host_install()` 以 Host 模式申请，`usb_host_uninstall()` 释放。
- PHY 占用互斥可阻止两个 stack 同时成功 install；应用层仍必须坚持先 deinit 当前角色，再 init 目标角色。

底层 API 支持释放后重新申请，但多轮 uninstall/install 是否完整复位 DWC 中断、FIFO、endpoint 和 session 状态不能只靠静态分析证明，必须上板循环验证。

## Kconfig 与启动行为

`main/Kconfig.projbuild` 使用 `choice EXAMPLE_USB_ROLE`，默认选择 `EXAMPLE_ENABLE_TEST_UVC`：

- Kconfig 保证 `EXAMPLE_ENABLE_TEST_USB` 与 `EXAMPLE_ENABLE_TEST_UVC` 不会同时为 `y`。
- `main/CMakeLists.txt` 只把所选 capability 的源文件加入本次构建，`main/idf_component.yml` 也只解析对应的 managed USB 依赖；因此一个固件只包含 CDC Device 或 UVC Host 其中一种 HIGH USB capability。
- 任一选项为 `y` 时，`app_system_init_task()` 创建 `test_usb_role` worker。
- `test_usb_role_init()` 正常启动角色固定为 `TEST_USB_ROLE_NONE`，不会在启动阶段占用 HIGH USB。
- role worker 运行时只允许在当前固件已编译的 capability 与 `TEST_USB_ROLE_NONE` 之间切换；未编译的另一角色请求返回 `ESP_ERR_NOT_SUPPORTED`，不会尝试动态安装不存在的 stack。
- 若 RTC no-init 标记表明热复位前 UVC 正在出流，role worker 会在初始化后例外地自动排队恢复 UVC。

因此，当前 `choice` 同时决定编译能力和依赖裁剪；实际硬件独占由 role worker 的生命周期顺序保证。CDC 与 UVC 的验收必须使用两个独立构建分别进行，不能在同一个固件中做 CDC ↔ UVC 运行时切换。

## CDC Device

`test_usb` 在 HIGH USB 上安装 TinyUSB Device 和一个 `TINYUSB_CDC_ACM_0` 接口：

- CDC RX 回调只投递事件，`usb_cdc_evt` worker 读取数据、输出 hex dump 并原样回显。
- line-state 回调记录 DTR/RTS。
- `test_usb_get_status()` 返回 attached、DTR/RTS、收发包数、字节数和最后错误。
- `test_usb_deinit()` 依次释放 CDC、TinyUSB、worker 和 queue，可在未初始化状态安全调用。

FULL USB 的 USJ 串口始终承担烧录和 IDF 日志；CDC 测试枚举出的第二个串口来自 HIGH USB，不要混用。

## UVC Host

### 生命周期

`test_uvc_init()` 安装以下资源，但不要求摄像头已经连接：

1. 以 `root_port_unpowered=true` 安装 controller[0] USB Host。
2. 创建 USB Host library event task 和 UVC driver task。
3. 安装 UVC client，随后开启 root port。
4. 申请 ESP32-P4 JPEG decoder、帧队列、解码缓冲和固定预览缓冲。
5. 优先申请 PPA SRM 缩放资源；失败时保留 CPU 缩放回退。

`test_uvc_run()` 是有界阻塞调用，等待设备、枚举能力并打开流。frame callback 只把带有 `stream_hdl` 和 stream `generation` 的消息投递给 worker；worker 完成 JPEG 解码、预览缩放和统计。stream callback 只记录断连事件，不在 USB callback 中直接关闭句柄；worker 在 stream mutex 下验证 generation、停止/关闭对应 stream，并归还该代次的全部帧。旧代次事件不能修改新 stream 的状态。`test_uvc_deinit()` 先停止接收、排空并归还所有应用持有帧，确认句柄和 held-frame 计数归零后，才卸载 UVC client/Host 并释放 JPEG、PPA、queue、task 和 buffer；归还或关闭失败时保留资源并报告错误，不能继续卸载 controller。

### 帧句柄与断连所有权

- 新 stream 生命周期发布时才递增 generation；frame callback 只把当前 generation 和 `stream_hdl` 带入消息、增加 held-frame 计数并将 frame handle 的所有权交给应用队列。队列满时立即归还该帧并回退 held-frame 计数，不把裸指针当成无主资源。
- worker 消费消息前同时匹配 `stream_hdl` 与 generation。过期消息只做归还，不参与解码、统计或状态迁移。
- 断连 callback 只投递“断连 + handle + generation”事件；worker 先停止接收，再排空队列、归还所有帧，最后关闭匹配句柄。关闭新代次 stream 的请求不能被旧断连事件抢先执行。
- `test_uvc_deinit()` 和分辨率切换都复用同一生命周期锁，只有在 `held_frames == 0` 且活动句柄已关闭后才释放 Host、JPEG/PPA 等下游资源。

### 能力枚举与分辨率切换

UVC 不再硬编码 FPS：

- 设备连接后读取 MJPEG frame descriptor，最多保存 `TEST_UVC_FRAME_INFO_MAX=24` 条能力。
- 默认偏好为 `1280x720`；若设备支持该分辨率，选择对应最高 FPS。
- 偏好不匹配时，从设备实际支持的 MJPEG 能力中选择降级项，并记录最终协商的宽、高和 FPS。
- `test_uvc_get_resolutions()` 向 UI 暴露去重后的分辨率列表。
- `test_usb_role_restart_uvc()` 将分辨率请求投递到 role queue；若 UVC 已运行，worker 调用 `test_uvc_change_resolution()` 停止当前流并用新能力重新打开，不在 LVGL 回调中执行阻塞操作。

### 解码与预览

- USB UVC 帧使用 PSRAM 缓冲，格式为 MJPEG；ESP32-P4 硬件 JPEG decoder 输出 RGB565。
- 固定预览尺寸为 `896x448`，保持源画面宽高比并居中，空白区域清零。
- 预览默认最多每 100 ms 更新一次。PPA 可用时走 PPA 缩放，否则走 CPU RGB565 缩放。
- `test_uvc_copy_preview()` 将最新预览复制到调用方缓冲；调用方带入上一帧 `frame_id` 时，没有新帧会返回 `ESP_ERR_NOT_FOUND`，避免重复整帧复制。
- 状态中保留接收、解码、丢帧、错误、queue 高水位、解码耗时、预览耗时、缩放后端和 fallback 次数，UI 只读取摘要，不持有 JPEG 或 USB 内部缓冲。

USB/JPEG/PPA 驱动负责其 DMA 边界，应用层仍显式维护自己写入的 PSRAM：UVC worker 更新预览缓冲后执行 `esp_cache_msync(..., DIR_C2M)`；UI 把预览复制到 LVGL image 自有缓冲后再次写回 cache，再 invalidate 图像对象。后续新增“CPU 写 PSRAM 后交 DMA 读取”的路径也必须在所有权交接点执行同类同步，不能依赖 LVGL 隐式处理。

## 角色管理与恢复

### 对外接口

`test_usb_role` 提供以下 capability：

- `test_usb_role_init()`：创建 queue 和固定在 core 1 的 role worker，正常启动保持 None。
- `test_usb_role_switch(target)`：异步切换到 None 或当前构建已包含的 CDC Device/UVC Host capability；请求未编译的角色返回 `ESP_ERR_NOT_SUPPORTED`。
- `test_usb_role_restart_uvc(h_res, v_res)`：异步请求 UVC 分辨率切换。
- `test_usb_role_current()` / `test_usb_role_get_status()`：返回当前/目标角色、切换状态，以及 CDC/UVC 摘要。`get_status()` 在模块尚未初始化或初始化失败时也返回可诊断快照；只有空指针参数返回 `ESP_ERR_INVALID_ARG`。

所有公开状态快照都带 `test_module_meta_t`：`generation` 只在成功建立新模块生命周期后递增，`updated_at_us` 在每次公开状态变化时更新；错误应通过 `module.last_error` 和 capability 的最后错误字段读取，不用 UI 自行推断。

普通角色切换严格执行：

1. 相同目标直接成功；切到 None 只释放当前角色。
2. deinit 当前 CDC 或 UVC，随后立即把当前角色记为 None。
3. init/run 目标角色。
4. 目标初始化失败时清理半初始化资源，保持 None 并记录 `last_switch_error`。

UVC 初始化成功但暂时没有摄像头时，Host 角色可以保持就绪；健康检查发现设备后来接入且 stream 未启动时，会再次调用 `test_uvc_run()`。

### 健康检查与分级恢复

role worker 每 `250 ms` 检查一次 UVC 状态。连续 `5 s` 没有收到视频帧会置位恢复请求，恢复不会从 UVC callback 或 frame worker 内直接卸载自身。

自动恢复按以下等级升级，每级最多尝试 3 次。同一级前两次失败后分别等待
500/1000 ms；第 3 次失败升级到下一级，并在 500 ms 后开始该级尝试：

1. `STREAM`：停止并重新打开 UVC stream。
2. `ROOT_PORT`：逻辑关闭/开启 DWC root port 后重新枚举和出流。
3. `STACK`：完整卸载并重建 UVC Host、DWC PHY、JPEG 和 PPA 资源。

成功出流后在 RTC no-init 内存写入标记；有意退出 UVC 时清除。热复位中断出流会保留标记，下一次启动自动请求 UVC，并在没有设备对象时从 root-port 级别开始恢复。所有等级耗尽后停止自动重试，UI 显示错误并提示重新插拔。

### VBUS 硬件边界

JC1060P470 原理图没有给 HIGH USB VBUS load switch 提供 ESP32-P4 GPIO enable。`usb_host_lib_set_root_port_power(false)` 只能停止 root port、断开逻辑会话并阻止重新枚举，不等价于摄像头 5V 真正掉电。

若摄像头固件死锁且必须掉电才能恢复，现有软件三级恢复无法保证成功，只能重新插拔，或后续增加 GPIO 控制的 load switch/支持端口供电控制的 Hub。

## 验证状态

| 路径 | 状态 | 说明 |
|------|------|------|
| FULL USB 烧录与日志 | 已使用 | USJ 固定功能，不属于应用层 TinyUSB |
| CDC Device echo 单路径 | 已上板验证 | 枚举、RX dump、回显和 line state 有既有记录 |
| UVC 收流 + JPEG 单路径 | 已上板验证 | MJPEG 收流和硬件解码日志有既有记录 |
| 设备能力枚举与偏好降级 | 已实现，待本轮上板确认 | 需用不同摄像头核对 descriptor 和降级结果 |
| UVC RGB565 预览、PPA/CPU 回退 | 已实现，编译验证通过 | 需测画面、帧率、耗时和长时间内存稳定性 |
| 当前 capability/None 重复切换 | 已实现，编译验证通过 | CDC 和 UVC 分别在各自构建中循环验证 DWC、PHY、task 和 buffer 是否完整释放 |
| 分辨率运行时切换 | 已实现，编译验证通过 | 需覆盖成功、设备不支持和切换中拔出设备 |
| stall 与三级恢复 | 已实现，编译验证通过 | 需注入停帧、拔插和恢复耗尽场景 |
| UVC 热复位自动恢复 | 已实现，编译验证通过 | 需验证 RTC no-init 标记和无真实 VBUS 断电边界 |

上板验收至少应分两套固件进行：CDC 构建覆盖 CDC → None 循环、枚举、回显和重复启停；默认 UVC 构建覆盖 UVC → None 循环、页面反复进出、所有摄像头分辨率逐项切换、流中拔插、流中热复位、停帧恢复和恢复耗尽提示。两套构建都应确认运行期间 `FULL USB` 日志不受影响。本轮文档对应的新增生命周期路径仅完成静态检查和编译验证，尚未完成上述上板回归。
