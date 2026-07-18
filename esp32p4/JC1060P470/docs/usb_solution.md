# USB 子系统当前方案

## 端口与硬件边界

JC1060P470 有两个能力不同的 USB Type-C 口：

| 端口 | 信号 | 当前用途 |
|------|------|----------|
| `FULL USB` | GPIO24/25，USB Serial/JTAG | 固定 CDC-ACM 烧录与日志，不属于应用 DWC OTG |
| `HIGH USB` | 封装专用脚 49/50，内部 UTMI HS PHY | DWC OTG controller[0]，可运行 HS Device 或 HS Host |

`HIGH USB` 的 controller[0] 同一时刻只能由一个角色占用。板上没有由 ESP32-P4 GPIO
控制的 VBUS load switch，因此关闭 root port 只能结束逻辑会话，不能保证摄像头 5 V
真正掉电。必须物理断电才能恢复的设备仍需重新插拔。

ESP32-P4 V1.0 还可把 controller[1] 的 FS OTG 路由到 GPIO26/27，但 GPIO27 已用于 LCD
复位，本工程不使用该路径。

## 编译能力与运行时所有权

`main/Kconfig.projbuild` 的 `choice EXAMPLE_USB_ROLE` 在编译期选择一种 HIGH USB 能力：

- 默认 `EXAMPLE_ENABLE_TEST_UVC=y`：编译 UVC Host。
- 可选 `EXAMPLE_ENABLE_TEST_USB=y`：编译 TinyUSB CDC Device。
- CMake 只加入所选源文件，managed component 依赖也按相同条件解析。

`test_usb_role` 始终先以 `None` 启动。UI 请求启用能力后，固定在 core 1 的 role worker
串行执行当前角色 deinit、确认 controller[0] 释放、再 init/run 目标角色。一个固件只能在
已编译能力与 `None` 之间切换，不能在同一固件内动态安装 CDC 和 UVC 两套栈。

角色切换失败时保留可诊断的 fault、最后错误和控制器所有权。若底层无法确认释放，模块
进入 ownership unknown 状态，只允许继续尝试释放，不允许安装另一套栈。

## CDC Device

`test_usb` 在 HIGH USB 上安装 TinyUSB Device 和一个 CDC-ACM 接口：

- RX callback 只投递事件，`usb_cdc_evt` worker 读取、调试级 hex dump 并原样回显。
- 状态快照包含 attach、DTR/RTS、收发包/字节数和队列丢弃计数。
- deinit 先停止 worker，再释放 CDC 和 TinyUSB；卸载失败时保留仍被 callback 引用的资源。

该 CDC 接口是 HIGH USB 枚举出的第二个串口，不能与 FULL USB 的烧录/日志口混用。

## UVC Host 生命周期

`test_uvc_init()` 完成以下资源申请：

1. 以 `root_port_unpowered=true` 安装 controller[0] USB Host。
2. 创建 USB Host library event task、UVC driver task 和应用 frame worker。
3. 安装 UVC client，随后开启 root port。
4. 创建 JPEG decoder、帧队列、RGB565 解码缓冲和 front/back 预览双缓冲。
5. 优先注册 PPA SRM client；失败时保留 CPU 缩放回退。

frame callback 只校验当前 stream generation、登记 frame 所有权并投递有界队列。worker
负责完整性检查、JPEG 解码、预览缩放和统计。队列满或 generation 过期的帧立即归还，
不会积压历史画面。

stream handle、callback context 和 held frame 具有同一代次所有权。只有
`uvc_host_stream_close()` 成功后才回收 context；close 或 frame return 失败时保留资源并
报告错误，禁止继续卸载仍可能触发 callback 的 Host 栈。

deinit 顺序为：停止接收、关闭 stream、排空并归还 held frame、关闭 root port、停止
worker、卸载 UVC client、等待 USB device 全部释放、停止 Host task、卸载 Host，最后释放
JPEG、PPA、queue、锁和 PSRAM buffer。

## 能力协商与分辨率

设备连接后枚举 MJPEG frame descriptor，最多保存 `TEST_UVC_FRAME_INFO_MAX=24` 条：

- 默认稳定偏好为 `1280x720`，在同分辨率中选择最高 FPS。
- 没有 720p 时，选择不超过 `1280x720` 的最大 MJPEG 档。
- 如果所有档位都超过稳定上限，选择设备提供的最小 MJPEG 档。
- UI 展示去重后的分辨率列表，仍允许用户手动选择 1080p 等高负载档。
- 分辨率切换由 role queue 串行处理，不在 LVGL callback 内执行阻塞 USB 操作。

UVC 使用 3 块固定 `1 MiB` MJPEG frame buffer。固定大小用于绕开异常 control probe
返回 `dwMaxVideoFrameSize=0` 后被误报为 `ESP_ERR_NO_MEM` 的情况，不代表任意摄像头的
MJPEG 帧都必然小于 1 MiB。

## 解码、预览与 Cache

硬件 JPEG decoder 输出 RGB565。两块预览缓冲均按面板最大尺寸 `1024x600 RGB565`
分配，总计约 2.34 MiB PSRAM：

- worker 在 back buffer 完成缩放和 cache write back 后，短暂加锁交换 front/back。
- UI 只复制稳定 front buffer；传入上一帧 `frame_id` 时，无新帧返回
  `ESP_ERR_NOT_FOUND`，不重复拷贝和重绘。
- 1920x1080、1280x720 等比缩放为 1024x576。
- 800x600、640x480、640x360 等低分辨率不放大，按原尺寸居中；UI 只复制有效区域。
- 720p 和 1080p 的 JPEG/PPA 处理最小间隔为 200 ms，即最多 5 次尝试/秒；更低分辨率
  不限流，由实际 JPEG、PPA、PSRAM 和 LVGL 吞吐决定发布速度。

解码前要求 SOI 为 `FFD8`，并在帧尾 512 字节窗口内找到 EOI `FFD9`。尾部截断帧不进入
硬件 decoder；SOI/EOI 完整但中段丢包的帧可能由 decoder 报错并被丢弃。

应用层所有“CPU 写 PSRAM、DMA 读取”的交接点都显式执行
`esp_cache_msync(..., ESP_CACHE_MSYNC_FLAG_DIR_C2M)`：worker 发布预览前一次，UI 将图像
复制到 LVGL 自有缓冲后再执行一次。不能依赖 LVGL 隐式维护 ESP32-P4 cache 一致性。

默认启用 PPA 预览；`EXAMPLE_UVC_ENABLE_PPA_PREVIEW=n` 只用于单变量 CPU 回退诊断。

## 健康检查与恢复

role worker 每 250 ms 调用 `test_uvc_poll_health()`。状态分别记录 frame callback 的
ingress 时间、worker 推进时间和当前 JPEG/PPA 阶段：

- 5 秒没有 ingress：`TRANSPORT_STALL`。
- ingress 继续但 worker 5 秒不推进：`WORKER_STALL`。
- stream control 操作超时：`CONTROL_TIMEOUT`，当前 EP0 实例视为不可信。
- 高负载档连续 4 次解码失败，或 5 秒窗口至少 12 次尝试且错误率达到 25%：请求稳定降级。

稳定降级优先在当前设备上把高分辨率 stream 切到 1280x720；失败后进入恢复。普通错误按
STREAM、ROOT_PORT、STACK 逐级升级，每级最多 3 次并采用 500/1000 ms 退避。transport、
worker 或 control timeout 直接要求 STACK 重建，以重新创建 EP0 control transfer 状态。

root-port 恢复先逻辑关闭端口并等待最多 2000 ms 的 device-gone 屏障，再关闭 stream。
未收到屏障时保留 handle 和 controller 所有权，不进行危险的强制卸载。成功出流时写入
RTC no-init 标记；正常退出清除标记，热复位中断出流后下次启动会自动请求恢复。

## 对外接口与状态

- `test_usb_role_init/deinit()`：创建或释放角色框架。
- `test_usb_role_switch()`：异步切换 `None` 与当前编译能力。
- `test_usb_role_restart_uvc()`：异步切换 UVC 分辨率。
- `test_usb_role_get_status()`：聚合角色、controller、CDC/UVC 和恢复摘要。
- `test_uvc_get_status()`：读取 ingress/received/decoded/published、错误、耗时、worker 阶段和恢复原因。
- `test_uvc_get_resolutions()`：读取设备实际枚举到的 MJPEG 分辨率。
- `test_uvc_copy_preview()`：复制稳定 RGB565 front buffer 快照。

所有查询接口只返回快照，不向 UI 暴露 USB、JPEG、PPA 或 DMA buffer 句柄。

历史实验、实测数据和当前待验收项统一记录在
[validation_status.md](./validation_status.md)，不在方案文档中用旧版本结果推断当前稳定性。
