# Companion Display

`companion_display` 将 Waveshare ESP32-S3-Touch-AMOLED-1.8 用作桌面伴侣显示屏。它通过 Wi-Fi 接收桌面、窗口或裁剪区域的 JPEG 画面；它不是操作系统的扩展显示器，也不需要安装显示驱动。

视频有两种模式：TCP 模式通过 WebSocket 可靠传输，适合首次联调与稳定网络；UDP 模式仍以 WebSocket/TCP 保持控制连接，视频分片改用 UDP，以丢弃不完整帧换取更低延迟。两种模式都采用最新画面优先策略，设备或采集端处理不过来时会替换待处理旧帧，而不是持续累积延迟。

## 固件

在仓库根目录执行：

```bash
./dev.sh build
./dev.sh -p /dev/ttyACM0 flash-monitor
```

第二条命令会烧录并打开 ESP-IDF 串口监视器。请按实际设备替换端口，例如 `/dev/ttyUSB0`。显示传输不再使用 USB，但 USB Serial-JTAG/串口仍用于烧录和日志。

首次启动时，连接设备创建的 `Companion-XXXX` Wi-Fi 热点，密码由 `CONFIG_COMPANION_AP_PASSWORD` 配置，默认是 `companion-display`。在浏览器打开 `http://192.168.4.1/`，填写目标 Wi-Fi 的 SSID 和密码。设备获取 DHCP 地址后，屏幕会显示 IP 地址和六位配对码。

默认端口为 HTTP/WebSocket `80` 和 UDP 视频 `5000`，可在 `menuconfig` 的 `Companion Display Wi-Fi` 中修改。当前不提供 mDNS 发现，请在 PC 客户端中直接输入设备 IP。

## PC 客户端

桌面发送端位于 `tools/desktop_client`，依赖 Rust 和 Node.js：

```bash
cd tools/desktop_client
npm install
npm run tauri:dev
```

输入形如 `ws://192.168.1.42/stream` 的地址和设备屏幕上的六位配对码，点击 `Check` 确认设备状态，选择采集源后启动。`Static color bars` 是不依赖屏幕采集权限的测试源，适合验证 JPEG、网络和显示链路。

客户端输出固定为 `368x448` JPEG，最多 30 FPS，JPEG 负载最多 100 KiB。默认使用 25 FPS、JPEG 质量 70；采集画面会等比例缩放，并以黑边补齐。Linux 的真实画面采集统一通过系统 Portal/PipeWire 对话框选择屏幕或窗口，不使用逐帧 XCap 抓屏；PipeWire 回调直接生成设备尺寸图像，避免复制完整桌面。状态栏会分别显示采集等待、编码和设备耗时及替换帧数；TCP 模式一次只允许一帧在途，避免可靠传输累积旧画面。

静态画面会复用最后一次成功编码的 JPEG，以 `1 FPS` 重复发送，并为每次发送生成新帧序号；画面恢复变化后立即回到用户设置的实时帧率。这样静态内容不会占用实时带宽，也不会使设备认为视频会话失活。UDP 模式同时每 2 秒保持一次 WebSocket 控制心跳；控制连接异常时客户端会显示 `Reconnecting control channel`，最多自动重连 5 次并重新协商 UDP session，成功后继续发送当前画面。

### TCP 与 UDP 的选择

| 模式 | 视频传输 | 适用场景 |
| --- | --- | --- |
| TCP（默认） | WebSocket 二进制消息 | 首次联调、弱网络、需要完整画面时 |
| UDP | 协商后的 UDP 分片 | 稳定局域网、重视低延迟、可接受偶发丢帧时 |

两种模式都必须保持 WebSocket 控制连接。UDP 不能独立建立视频会话；设备只接受已协商会话中的视频分片。

### 低延迟验收

实时画面推荐使用 UDP、25 FPS 和 JPEG 质量 70。Linux 必须选择 `Portal` 系统对话框来源；`Static color bars` 只用于验证编码、网络和显示链路，不能代表真实桌面采集延迟。

状态栏中的 `Capture` 是等待最新采集图像的时间，`Encode` 是 JPEG 编码与帧封装时间，`Device` 是设备报告的排队、解码和 LCD 提交时间。三项通常应各自低于 40 ms；任一项持续超过该值时会显示 `Performance limited`。`Capture` 超过 500 ms 时，优先停止流并重新建立 Portal 共享会话，而不是调整设备端参数。

实机运行时，设备状态中的 `completedFrames` 与 `submittedFrames` 应持续增长且接近；`reassemblyTimeouts` 不应持续快速增长。出现少量 UDP 丢帧是允许的，但画面不应持续落后。

## 联调与排障

- 先用 TCP 和 `Static color bars` 验证基础链路。设备显示 `TCP VIDEO` 或 `RECEIVING FRAME` 即表示已进入视频接收。
- UDP 无画面时，查看设备串口的 `UDP stats`。`rx` 增长说明数据已到设备；`accepted` 为零时查看 `last` 拒绝原因；`submitted` 增长才表示完整帧已进入显示管线。发送端会以 1 ms 间隔发送分片，并限制单帧发送在 120 ms 内；设备 UDP 邮箱为 24 包，持续 `timeout` 仍表示 Wi-Fi 丢包或发送端负载过高。
- `accepted` 增长而 `completed` 不增长：完整帧有分片丢失、被较新帧替换，或发送端未在预算内发完。`completed` 增长而 `submitted` 不增长：完整帧已收齐，但 `CDP1` 校验或显示管线提交失败。
- `source_ip`：UDP 来源地址与已绑定地址不一致。PC 必须将 UDP socket 绑定到 WebSocket 控制连接使用的本机 IPv4；重新启动流可创建新会话。
- `bad CRC`：完整 `CDP1` 帧的 JPEG CRC 校验失败。检查发送端和固件是否使用同一协议版本，或先改用 TCP 确认编码链路。
- `device_busy`：设备已有活动控制会话或显示管线仍被占用。先在旧客户端点击 Stop；若连接异常断开，关闭旧 WebSocket 或重启设备以释放会话。
- `UDP REJECTED`、`UDP DROPPED` 或 `SESSION EXPIRED`：分别表示分片被拒绝、重组超时丢帧或 5 秒未收到 UDP 数据。UDP 丢帧不重传，持续异常时改用 TCP。
- `SESSION EXPIRED` 在 UDP 模式只表示媒体暂时没有新包；设备会保留 WebSocket 与 UDP session，静态 1 FPS 保活帧或恢复后的画面可继续使用原 session。只有 Stop 或 WebSocket 关闭会释放会话。
- `WebSocket peer closed`：对端已显式关闭控制连接；这会释放会话。若客户端显示 `Reconnecting control channel`，它会以同一 PC IPv4 自动替换设备端遗留的旧会话，并使用新 UDP session 恢复；重连 5 次仍失败时检查 Wi-Fi、IP 与配对码。
- `PC` 或 `Device` 耗时持续接近帧间隔：降低手动 FPS 或 JPEG 质量。`Replaced` 增长说明系统正在主动跳过旧画面以维持低延迟；这比画面持续落后是预期且更适合实时显示的行为。
- Linux 真实画面延迟异常时，重新启动流并在 Portal 对话框重新选择共享对象。若 `Capture`、`Encode` 或 `Device` 任一项超过 40 ms，客户端会显示 `Performance limited`；先保持默认 25 FPS、质量 70，再依据该指标逐步降低配置。
- 本次低延迟优化使用控制协议 v2，PC 客户端和设备固件必须成对更新；客户端检查到非 v2 设备时会拒绝启动。

完整的控制消息、二进制帧和状态字段定义见 [PROTOCOL.md](PROTOCOL.md)。本版本仅适合可信局域网：配对码和 UDP 会话参数不提供传输加密。
