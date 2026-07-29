# Companion Display 设备端实现

本文面向固件使用者和维护者，说明 ESP32-S3 设备端的构建、模块边界、运行流程、并发模型和诊断方式。线上字段和二进制格式以[公共通信协议](../../../PROTOCOL.md)为准。

## 构建、烧录与配置

设备端基于 ESP-IDF，目标芯片为 ESP32-S3，默认启用 16 MiB Flash、Octal PSRAM、240 MHz CPU 和 2,000,000 baud 控制台。

```bash
./dev.sh build
./dev.sh ports
./dev.sh -p /dev/ttyACM0 flash-monitor
./dev.sh menuconfig
```

`dev.sh` 通过 `eim` 运行 `idf.py`。`flash`、`monitor` 和 `flash-monitor` 需要有效串口；`-p` 与 `PORT` 环境变量都可以覆盖默认 `/dev/ttyACM0`。

主要项目配置如下：

| 配置 | 工程默认值 | 作用 |
| --- | --- | --- |
| `CONFIG_COMPANION_HTTP_PORT` | `80` | 配网页面、状态接口和 WebSocket 控制端口 |
| `CONFIG_COMPANION_UDP_PORT` | `5000` | UDP 视频接收端口 |
| `CONFIG_COMPANION_AP_PASSWORD` | `companion-display` | 设备 SoftAP 密码 |
| `CONFIG_COMPANION_FRAME_TIMEOUT_MS` | `10000` | 流启动后未收到完整帧的会话超时 |
| `CONFIG_COMPANION_FRAME_LIMIT_KB` | `100` | JPEG payload 最大尺寸 |
| `CONFIG_COMPANION_FRAME_BUFFER_COUNT` | `2` | 工程默认分配的 PSRAM JPEG 缓冲数量 |
| `CONFIG_COMPANION_DRAW_TASK_PRIORITY` | `5` | JPEG 解码和显示任务优先级 |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE` | `48` | lwIP UDP 接收邮箱容量 |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | `16` | Wi-Fi 静态接收缓冲数量 |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | `48` | Wi-Fi 动态接收缓冲数量 |
| `CONFIG_ESP_WIFI_RX_BA_WIN` | `16` | AMPDU RX BA 窗口 |

`Kconfig` 中帧缓冲数量的菜单回退值为 3，但当前 `sdkconfig.defaults` 明确设为 2，因此新工程配置以 2 为默认。显示分辨率、GPIO、I2C、LCD PCLK 和电源时序位于 `Board AMOLED 1.8` 配置组。

## 启动流程

`app_main()` 按固定顺序初始化：

```text
芯片与 Flash 信息
  -> subscreen_display_init
  -> 显示主信息界面
  -> frame_pipeline_init
  -> transport_wifi_init
```

显示先于网络启动，因此配网、连接和传输状态可以直接反馈到屏幕。任何关键初始化失败都会通过 `ESP_ERROR_CHECK` 终止启动并在串口输出错误。

## 模块结构

| 模块 | 职责 |
| --- | --- |
| `board_amoled_1_8` | 板级 I2C、PMU、SH8601 LCD、FT3168 触摸、SD、RTC、IMU 和音频设备接口 |
| `subscreen_display` | 主信息界面、状态栏、JPEG 解码、RGB565 转换和 LCD 提交 |
| `frame_pipeline` | `CDP1` 校验、PSRAM 帧缓冲、传输互斥、最新帧替换和绘制任务 |
| `touch_input` | 触摸轮询任务：将 `board_touch_read()` 的采样事件化（down/move/up），批量回调；不感知网络 |
| `transport_wifi` | AP+STA 配网、NVS、HTTP、WebSocket 控制、TCP 视频、UDP 重组、会话生命周期和 `touch_events` 发送 |
| `main` | 按依赖顺序组合上述模块 |

板级组件暴露的音频、SD、RTC 和 IMU 接口目前不是视频链路的组成部分，相关 capability 为后续扩展预留。

### 触摸事件上报

`touch_input` 在 `transport_wifi_init()` 中启动一个 10 ms 周期的轮询任务（优先级 3，栈 4 KiB）。驱动内部由中断窗口门控，空闲时轮询开销接近于零；任务同时缓存触摸硬件就绪状态供 `/api/status` 与 `hello` 协商无锁读取。事件生成规则：按下与释放立即成批发送，移动坐标合并后约 15 ms 一批（≤8 事件）；I2C 读取失败时会合成一次 `up`，避免主机端按键悬挂。

`transport_wifi` 在会话满足“已订阅 `features.touch_events` + 触摸硬件就绪 + `stream_started`”时，把事件批经 `httpd_queue_work` + `httpd_ws_send_frame_async` 异步发出（与 `frame_presented` 相同模式），批序号 `seq` 每会话从 0 递增，队列失败会丢批并留下序号空洞供 PC 检测。会话释放（stop、断开、超时）后 gate 立即关闭。

## 显示链路

### 板级显示

`board_display` 负责 PMU 供电、TCA9554 扩展 GPIO、面板复位、SH8601 初始化、显示开启和背光设置。默认物理分辨率为 `368x448`，LCD PCLK 为 40 MHz。

面板颜色数据使用 SH8601 所需的 RGB565 线格式。CPU 逻辑颜色常量在提交前交换字节；详细约束见[显示颜色格式](display-color-format.md)。

### 主信息界面

`subscreen_display` 不依赖 LVGL，而是使用内置 5x7 字体和基本绘图函数生成信息卡片。界面显示：

- Wi-Fi 状态；
- 设备 IPv4 地址；
- 六位配对码；
- 当前控制、视频或错误状态。

流结束后，模块同步等待正在进行的 LCD 传输完成，再重绘主界面，防止旧视频 DMA 在占位界面之后覆盖屏幕。

### JPEG 解码与 LCD 提交

设备使用 `esp_jpeg` 将 JPEG 解码为 RGB565。解码输出按块提交给面板，并记录四段设备耗时：

- `jpeg_decode_us`：JPEG 解码耗时；
- `lcd_wait_us`：等待前一次 LCD 传输完成的耗时；
- `lcd_submit_us`：提交当前画面的调用耗时；
- `previous_dma_us`：上一笔 DMA 从提交到完成的耗时。

设备只接受与面板尺寸一致的 `368x448` 帧。缩放、裁剪和补边由 PC 客户端完成。

## 帧管线与并发

`frame_pipeline` 在 PSRAM 中分配 `CONFIG_COMPANION_FRAME_BUFFER_COUNT` 个 JPEG payload 缓冲，在内部内存保存帧元数据和同步对象。接收端提交前校验：

- `CDP1` magic、版本、格式和 24 字节头长度；
- payload 非空且不超过配置上限；
- CRC-32 与 JPEG payload 一致；
- 当前 TCP/UDP 来源已经获得管线所有权。

`filled` 队列容量固定为 1。提交新帧时，如果队列中已有未绘制帧，就复用或归还旧缓冲并增加 `replaced_frames`。绘制任务取到帧后还会清空队列中更晚到达的旧项，只绘制最新项。

管线使用两种机制防止旧会话污染新界面：

- `active transport` 保证 TCP 和 UDP 不会同时占用显示管线；
- `generation` 在 acquire/release 时递增，使旧会话排队帧失效。

`frame_pipeline_release()` 会禁止继续提交、同步等待当前绘制临界区，并清空待绘制帧。已经开始的 LCD 绘制允许完成，然后设备才返回主界面。

## Wi-Fi、HTTP 与持久化

设备始终使用 `WIFI_MODE_APSTA`：SoftAP 用于配网，STA 用于加入局域网。AP 名称由 MAC 地址后两字节生成，格式为 `Companion-XXXX`，默认频道 1，最多允许两个 AP 客户端。

NVS namespace 为 `companion`：

| Key | 内容 |
| --- | --- |
| `ssid` | 目标 Wi-Fi SSID |
| `password` | 目标 Wi-Fi 密码 |
| `token` | 六位配对码 |

配对码首次启动时随机生成并持久化。清除 NVS 会同时清除配对码和 Wi-Fi 凭据。

HTTP 服务提供：

| 路径 | 方法 | 用途 |
| --- | --- | --- |
| `/` | GET | 简单的 Wi-Fi 配网页面 |
| `/api/wifi` | POST | 接收表单字段 `ssid`、`password`，保存并连接 STA |
| `/api/status` | GET | 返回设备、网络、传输、性能和 UDP 统计 JSON |
| `/stream?token=PAIRING_CODE` | GET/Upgrade | 建立 WebSocket 控制连接 |

Wi-Fi 断开后固件会持续调用 `esp_wifi_connect()` 重连。当前没有删除凭据、重置配对码或 mDNS 发现接口。

## 控制会话与视频接收

设备同时只允许一个控制会话和一个视频流。WebSocket 建连先验证查询参数 `token`：错误返回 HTTP `401`，其他来源已有活动会话时返回 `403`。同一 IPv4 的新连接可以替换遗留旧会话，用于 PC 端控制重连。

### TCP 视频

TCP 模式将完整 `CDP1` 作为 WebSocket 二进制消息接收。完整帧成功绘制后，设备异步发送 `frame_presented`。PC 端等待该回执后才发送下一帧，从发送端限制可靠连接中的积压。

### UDP 视频

UDP 模式在 `hello_ack` 中返回随机 `session_id`、`session_token`、端口、最大 payload 和 `latest_frame_ack` 流控能力。只有 JPEG `CDU1` 分片走 UDP；协商、心跳、触摸及 `udp_feedback` 全部沿用 TCP/WebSocket。设备在整帧成功提交、不可恢复或提交失败后立即反馈对应 sequence，PC 同时只允许一帧在途。

设备只维护一个重组缓冲和一张 128 bit 分片位图。出现新 sequence、布局变化或重组超时时，未完成旧帧会被丢弃。完整帧重组后仍需通过内层 `CDP1` 长度、CRC 和帧管线检查。

UDP 超时分两级：

- 5 秒没有 UDP 包：显示 `SESSION EXPIRED`，清除未完成重组，但保留 WebSocket 会话和 UDP session；后续合法分片可恢复。
- 默认 10 秒没有任何完整帧：发送尽力而为的 `stream_stopped`，关闭控制连接，释放显示管线并显示 `STREAM TIMEOUT`。

Stop、WebSocket CLOSE 或底层 TCP 断开也会立即释放会话并返回主界面。

## 状态与指标

`/api/status` 中的关键诊断关系：

```text
rxPackets
  -> acceptedFragments
  -> completedFrames
  -> submittedFrames
  -> performance.sequence
```

- `rxPackets`：UDP socket 收到的数据报数量；
- `acceptedFragments`：通过 UDP 头、会话和来源校验的数据或 FEC 分片；
- `completedFrames`：所有分片均已收齐；
- `submittedFrames`：内层 `CDP1` 已成功提交帧管线；
- `submitFailures`：完整帧在长度、CRC、状态或缓冲阶段失败；
- `reassemblyTimeouts`：不完整帧超时或被新帧替换；
- `fecPackets` / `fecRecoveredFragments`：收到的 XOR 校验分片和成功恢复的数据分片；
- `fecUnrecoverableFrames`：同一校验组丢失多片等原因导致无法恢复的帧；
- `lastReject`：最近一次 UDP 头层拒绝原因；
- `lastOutcome` / `lastOutcomeSequence`：最近整帧结果及对应 sequence；
- `lastReassemblyUs`：最近整帧完成或放弃前的重组耗时；
- `droppedFrames`：会话级 UDP 丢帧累计；
- `performance`：最近一次成功显示帧的设备端耗时。

## 诊断与排障

| 现象或状态 | 判断与处理 |
| --- | --- |
| `BAD PAIRING CODE` / HTTP 401 | 使用屏幕当前显示的六位配对码 |
| `DEVICE BUSY` / HTTP 403 | 停止另一客户端；同一 PC 重连会按来源 IPv4 替换旧会话 |
| `BAD HEADER` | PC 与固件帧版本、尺寸或 payload 上限不一致 |
| `BAD CRC` | 内层 JPEG CRC 不一致；先改用 TCP 排除 UDP 分片问题 |
| `NO BUFFER` | 绘制占用全部缓冲且没有可替换待处理帧；降低 FPS 或质量 |
| `UDP REJECTED` | 查看串口 `UDP rejected` 的 `reason`、来源 IP 和 session |
| `UDP DROPPED` | 分片未在重组窗口内收齐，或被更新 sequence 替换 |
| `SESSION EXPIRED` | 5 秒未收到 UDP 包，会话仍可由合法分片恢复 |
| `STREAM TIMEOUT` | 默认 10 秒没有完整帧，控制连接和管线已释放 |

串口每秒输出一次活动 UDP 会话摘要。正常情况下 `completed` 与 `submitted` 持续增长且接近；`accepted` 增长但 `completed` 不增长表示分片丢失，`completed` 增长但 `submitted` 不增长表示内层完整帧或管线校验失败。

## 安全与扩展边界

六位配对码只保护 WebSocket 握手。UDP 依赖随机 session 值和来源 IPv4 绑定降低误收风险，但不提供加密、强认证或防重放。设备应运行在可信局域网。

触摸事件仅在配对成功的控制会话内、且客户端显式订阅后发送，设备不解释手势语义。新增文字或音频能力时，需要先在[公共通信协议](../../../PROTOCOL.md)定义消息、鉴权、生命周期、失败处理和兼容策略，再实现设备端与 PC 端代码。
