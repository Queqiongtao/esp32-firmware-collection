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
4. 申请 ESP32-P4 JPEG decoder、帧队列、解码缓冲和 front/back 预览双缓冲。
5. 优先申请 PPA SRM 缩放资源；失败时保留 CPU 缩放回退。

`test_uvc_run()` 是有界阻塞调用，等待设备、枚举能力并打开流。frame callback 只把带有 `stream_hdl` 和 stream `generation` 的消息投递给 worker；worker 完成 JPEG 解码、预览缩放和统计。stream callback 只记录断连事件，不在 USB callback 中直接关闭句柄；worker 在 stream mutex 下验证 generation、停止/关闭对应 stream，并归还该代次的全部帧。旧代次事件不能修改新 stream 的状态。`test_uvc_deinit()` 先停止接收、排空并归还所有应用持有帧，确认句柄和 held-frame 计数归零后，才卸载 UVC client/Host 并释放 JPEG、PPA、queue、task 和 buffer；归还或关闭失败时保留资源并报告错误，不能继续卸载 controller。

### 帧句柄与断连所有权

- 新 stream 生命周期发布时才递增 generation；frame callback 只把当前 generation 和 `stream_hdl` 带入消息、增加 held-frame 计数并将 frame handle 的所有权交给应用队列。队列满时立即归还该帧并回退 held-frame 计数，不把裸指针当成无主资源。
- worker 消费消息前同时匹配 `stream_hdl` 与 generation。过期消息只做归还，不参与解码、统计或状态迁移。
- 断连 callback 只投递“断连 + handle + generation”事件；worker 先停止接收，再排空队列、归还所有帧，最后关闭匹配句柄。关闭新代次 stream 的请求不能被旧断连事件抢先执行。
- `uvc_host_stream_stop()` 的 control transfer 超时时，在途请求不会被 UVC 组件取消；应用必须保留 stream 句柄，禁止继续 close。root-port 恢复先逻辑停端口，并等待 USBH 完成 EP0 flush/dequeue 后发出的 device-gone 事件，再关闭句柄。物理断连同样以 device-gone 作为无需再次发送 `SET_INTERFACE` 的关闭屏障。
- `test_uvc_deinit()` 和分辨率切换都复用同一生命周期锁，只有在 `held_frames == 0` 且活动句柄已关闭后才释放 Host、JPEG/PPA 等下游资源。

### 能力枚举与分辨率切换

UVC 不再硬编码 FPS：

- 设备连接后读取 MJPEG frame descriptor，最多保存 `TEST_UVC_FRAME_INFO_MAX=24` 条能力。
- 默认偏好为 `1920x1080`；若设备支持该分辨率，选择对应最高 FPS。
- 偏好不匹配时，从设备实际支持的 MJPEG 能力中选择降级项，并记录最终协商的宽、高和 FPS。
- `test_uvc_get_resolutions()` 向 UI 暴露去重后的分辨率列表。
- `test_usb_role_restart_uvc()` 将分辨率请求投递到 role queue；若 UVC 已运行，worker 调用 `test_uvc_change_resolution()` 停止当前流并用新能力重新打开，不在 LVGL 回调中执行阻塞操作。

### 解码与预览

- USB UVC 帧使用 PSRAM 缓冲，格式为 MJPEG；ESP32-P4 硬件 JPEG decoder 输出 RGB565。
- UVC 使用 3 块固定 `1 MiB` MJPEG frame buffer。该摄像头正常 probe 的 `dwMaxVideoFrameSize` 为 `1048576`；若 EP0 短传输导致 probe 结果中的 frame size 变为 0，继续采用驱动协商值会执行零字节分配并误报 `ESP_ERR_NO_MEM`，因此应用不再把 buffer 大小交给该字段决定。
- 默认协商稳定档 `1280x720` 的最高可用 FPS；若摄像头没有该档，优先选择不超过 `1280x720` 的最大 MJPEG 分辨率，若所有档位都超过上限则选择设备的最小 MJPEG 档。UI 仍保留 1080p 手动选择能力。
- 两块固定预览缓冲均为面板全屏 `1024x600 RGB565`，合计约 2.34 MiB PSRAM。worker 在不持有预览锁时将画面等比缩放到 back buffer，完成 cache 同步后短暂加锁交换 front/back；UI 只复制稳定的 front buffer。每块缓冲独立记录有效画面区域，仅在首次使用或宽高比变化时清零空白区域。
- 摄像头仍按协商帧率传输，应用侧限制 JPEG/PPA 预览处理频率：`1280x720` 最小间隔 `100 ms`（最高 10 次/秒），`1920x1080` 最小间隔 `200 ms`（最高 5 次/秒），更低分辨率不限制。失败尝试同样占用时间窗口，被限流的完整帧计入 `skipped` 并立即归还，避免损坏帧连续冲击 JPEG、PPA、PSRAM 和共享 DMA2D。
- 处理速度低于相机输入时由既有有界队列丢弃旧帧，不累积历史画面。UI 以 `33 ms` 周期查询最新 `frame_id`，没有新帧时不复制、不触发 LVGL 重绘。
- `1920x1080`、`1280x720`、`640x360` 等比输出为 `1024x576`；`1280x960`、`800x600`、`640x480` 等比输出为 `800x600`。所有分辨率使用同一路径，由 JPEG、PPA、PSRAM 和显示实际吞吐决定帧率，不硬编码目标值。
- 上板基线（2026-07-17，双缓冲改动前）：1920×1080 解码约 21~34 ms、PPA 约 84~103 ms、屏幕约 3 fps；640×360 解码约 1.5~5.6 ms、PPA 约 15~19 ms、屏幕约 7~8 fps。旧路径的 worker 与 UI 均受 `100 ms` 周期限制，且 PPA 全程持有预览锁；双缓冲及解除限流后的结果仍需重新上板验证。
- 双缓冲解除限流实测（2026-07-17）：1080p 启动后硬件解码错误快速增长至 16 次，并出现 `dma2d_force_end`；随后收流超时进入 root-port 恢复。后续日志确认重开流时的 `ESP_ERR_NO_MEM` 实际伴随 `Not enough memory for frame buffers 0`，根因是失败 probe 提供零尺寸，并非已证明 PSRAM 总量耗尽。固定 1 MiB frame buffer 后仍需上板循环复测。
- 解码前先做 MJPEG 完整性校验：头部 SOI（FFD8）+ 尾部 `TEST_UVC_MJPEG_EOI_SEARCH_WINDOW=512` 字节窗口内存在 EOI（FFD9）。尾部截断帧计入 `truncated` 并直接归还，不进硬件解码器。
- PPA 可用时走 PPA 缩放，否则走 CPU RGB565 缩放。1080p 源经 PPA SRM 缩放实测约 90 ms/次，受 PPA 引擎吞吐（约 23 MPix/s）限制，属常数开销，不能靠减少总线争用优化。
- `test_uvc_copy_preview()` 在预览锁内将稳定的 front buffer 复制到调用方缓冲；生产者只能在复制结束后把旧 front 作为下一块 back 使用。调用方带入上一帧 `frame_id` 时，没有新帧会返回 `ESP_ERR_NOT_FOUND`，避免重复整帧复制。
- 状态中保留接收、解码、发布、错误、截断、丢帧、queue 高水位、解码耗时、预览耗时、缩放后端和 fallback 次数；五秒统计的全部流量/错误计数均输出窗口增量，通过 `received/decoded/published` 区分输入、解码和预览生产速度，UI 只读取摘要，不持有 JPEG 或 USB 内部缓冲。

### ISOC 丢包与已知残余坏帧

`usb_host_uvc` v2.5.1 的等时回调对 `SKIPPED/TIMED_OUT` 包只跳过、不作废当前帧（`uvc_isoc.c`），丢包的 MJPEG 帧仍按完整帧上交应用。按项目约束不修改 `managed_components/`，应用层采取以下绕过：

- ISOC URB 配置为 4×20 KB（10 ms 等时缓冲，占内部 DMA RAM 80 KB），实证可用。注意 HCD 的 ISOC qTD 环只有 64 槽且每管道双缓冲调度：单 URB 不得超过 61 包，实测 8×20 KB 会令 ISOC 调度绕死、一帧不收，并进一步诱发 CTRL 超时和 usbh 断言崩溃复位；未上板验证不得加深。
- 尾部截断帧由完整性校验拦截；中段丢包但 SOI/EOI 完好的帧无法在解码前识别，进入硬件解码器后报 data-unit 数与分辨率不匹配，由 `s_decode_errors` 统计并节流告警。`jpeg.decoder` 标签日志已静默避免逐帧刷屏；`dma2d` 标签与显示路径共用，保持可见。
- 上板实测残余坏帧约占解码尝试的 15%，且该比例在多种负载配置（预览频率、按需解码、URB 深度）下基本不变，说明与应用侧 PSRAM 争用无关。坏帧被静默丢弃，预览由下一帧补上，画面连续性不受影响。
- 迁核甄别实验（2026-07-16，已回退）：USB 栈整体迁 core 0——role worker（`usb_host_install()` 在调用核上分配 USB-DWC ISR，其所在核由 `test_usb_role.c` 的 `TEST_USB_ROLE_TASK_CORE_ID` 决定）+ USB lib task + UVC driver task 均 1→0，解码 worker 留 core 1。结果坏帧占比约 20%（errors≈5.5/5s vs 基线 3.5），无改善，**排除 core 1 周期性中断遮蔽 USB ISR 的假设**。
- 剩余怀疑指向摄像头/线缆/UTMI PHY 链路信号完整性，待硬件甄别：换 USB 线缆、同一摄像头+线缆接 PC 验证源头帧质量（如 `ffmpeg -f v4l2 -input_format mjpeg` 采集后检查坏帧）、换另一颗摄像头。零代码软件旁证：UVC 页面运行时切 720p，若坏帧占比随帧字节数等比下降为随机每包丢损特征（链路级），保持不变则为帧结构性问题。

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
1080p 下 `jpeg_decoder_process()` 连续失败 4 次也会置位恢复请求并暂停接收。另有 5 秒统计窗口保护：窗口内至少完成 12 次硬解尝试且错误率达到 25% 时预防性降级，避免零散成功帧不断清零连续错误计数，最后拖到 EP0 已失去响应才处理。稳定降级首先复用仍在运行的高分辨率 stream handle，就地停止 1080p、选择 `1280x720` 并重新启动；成功时不关闭设备、不重新枚举。只有就地切换失败才进入 `ROOT_PORT` 恢复；已经关闭或已经协商到稳定档的 stream 不再重复执行 `1280x720 -> 1280x720`。成功降级后状态和 UI 记录自动降级次数；用户手动选择分辨率会清除此状态。

稳定降级的就地格式切换是恢复前置步骤，不单独占用恢复等级；失败后直接进入
`ROOT_PORT`。其余自动恢复按以下等级升级，每级最多尝试 3 次。同一级前两次
失败后分别等待 500/1000 ms；第 3 次失败升级到下一级，并在 500 ms 后开始该级尝试：

1. `STREAM`：停止并重新打开 UVC stream。
2. `ROOT_PORT`：逻辑关闭/开启 DWC root port 后重新枚举和出流。
3. `STACK`：完整卸载并重建 UVC Host、DWC PHY、JPEG 和 PPA 资源。

若 `STREAM` 级停止操作返回 control timeout，则不重复执行危险关闭，直接升级到
`ROOT_PORT`。该级在最多 `2000 ms` 内等待 device-gone；超时则保留句柄和 controller
所有权并报告失败，不强行卸载 USB Host。

打开 stream 时，`ESP_ERR_NOT_FINISHED`、`ESP_ERR_TIMEOUT` 和 `ESP_ERR_INVALID_RESPONSE`
按控制端点瞬时错误处理，在同一枚举代次内最多重试 3 次、间隔 500 ms；真实
`ESP_ERR_NO_MEM` 不在原 stream 上重复分配。日志同时记录 frame buffer 配置和
PSRAM free/biggest，区分总量不足、连续块不足与 probe 零尺寸。

应用传给 UVC 回调的 stream context 在 `uvc_host_stream_close()` 成功前保持固定；close
失败时同时保留 stream handle 和 context，成功时驱动已从内部链表移除并释放 stream，
此后才回收 context 槽位。这样既不让旧回调引用被提前复用的上下文，也避免多次恢复后
永久耗尽固定 context pool。

健康检查分别记录 frame callback 的 `ingress` 时间与 worker 的处理阶段/推进时间：
`ingress` 停止为 transport stall；`ingress` 继续但 worker 超过 5 秒不推进为
JPEG/PPA worker stall。该判断由 USB role worker 执行，不依赖可能已经停止的 UVC worker。
720p 上板日志中，1 MiB frame buffer 仍出现 overflow，同时伴随 `invalid MJPEG SOI`
和约 19%~36% 的 JPEG 错误率，说明异常来自 ISOC 包/EOF/FID 丢失后的错误拼帧，
不是 PSRAM 不足；`dma2d_force_end` 是损坏 JPEG 进入硬解后的结果。

transport/worker stall 会先逻辑关闭 root port、等待 device-gone 并关闭 stream，再完整
重建 UVC/USB Host Stack。任何 stream stop、格式切换或打开过程返回
`ESP_ERR_TIMEOUT` 后，当前共享 EP0 control transfer 被视为不可信，不再执行同一实例的
STREAM/ROOT_PORT 重试。完整 Stack 重建会重新创建 control transfer 及其完成信号量，
避免旧完成状态造成后续 PROBE/COMMIT 连锁失败。

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
| UVC 1080p 出流 + 按需解码 | 已上板验证（2026-07） | 1920x1080@30fps 协商命中偏好；按需解码后 5s 窗口 decoded≈20、errors≈3.5（约 15% 解码尝试为链路级坏帧，静默丢弃）；无 uvc-isoc 告警和 buffer overflow |
| 设备能力枚举与偏好降级 | 偏好命中已上板验证 | 1920x1080@30fps 命中记录在案；降级路径需用不支持偏好的摄像头核对 |
| UVC RGB565 预览、PPA/CPU 回退 | PPA 路径已上板验证 | 1080p 源 PPA 缩放约 90 ms/次，letterbox 预览正常；CPU 回退和长时间内存稳定性未验证 |
| 当前 capability/None 重复切换 | 已实现，编译验证通过 | CDC 和 UVC 分别在各自构建中循环验证 DWC、PHY、task 和 buffer 是否完整释放 |
| 分辨率运行时切换 | 已实现，编译验证通过 | 需覆盖成功、设备不支持和切换中拔出设备 |
| stall 与三级恢复 | 已实现，编译验证通过 | 需注入停帧、拔插和恢复耗尽场景 |
| USB Host 1.5.0 单变量试验 | 已升级，编译验证通过 | `usb_host_uvc` 保持 2.5.1；官方 1.5.0 changelog 未声明 ISOC 修复，需用 720p 连续运行数据判断是否有效 |
| UVC 热复位自动恢复 | 已实现，编译验证通过 | 需验证 RTC no-init 标记和无真实 VBUS 断电边界 |

上板验收至少应分两套固件进行：CDC 构建覆盖 CDC → None 循环、枚举、回显和重复启停；默认 UVC 构建覆盖 UVC → None 循环、页面反复进出、所有摄像头分辨率逐项切换、流中拔插、流中热复位、停帧恢复和恢复耗尽提示。两套构建都应确认运行期间 `FULL USB` 日志不受影响。本轮文档对应的新增生命周期路径仅完成静态检查和编译验证，尚未完成上述上板回归。
