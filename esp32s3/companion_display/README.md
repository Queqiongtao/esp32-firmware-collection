# Companion Display

`companion_display` 将 Waveshare ESP32-S3-Touch-AMOLED-1.8 用作桌面伴侣显示屏。它通过 Wi-Fi 接收桌面、窗口或裁剪区域的 JPEG 画面；它不是操作系统的扩展显示器，也不需要安装显示驱动。

视频有两种模式：TCP 模式通过 WebSocket 可靠传输，适合首次联调与稳定网络；UDP 模式仍以 WebSocket/TCP 保持控制连接，视频分片改用 UDP，以丢弃不完整帧换取更低延迟。两种模式都采用最新画面优先策略，设备或采集端处理不过来时会替换待处理旧帧，而不是持续累积延迟。

## 固件

在 `esp32s3/companion_display` 工程目录执行：

```bash
./dev.sh build
./dev.sh -p /dev/ttyACM0 flash-monitor
```

第二条命令会烧录并打开 ESP-IDF 串口监视器。请按实际设备替换端口，例如 `/dev/ttyUSB0`。显示传输不再使用 USB，但 USB Serial-JTAG/串口仍用于烧录和日志。

首次启动时，连接设备创建的 `Companion-XXXX` Wi-Fi 热点，密码由 `CONFIG_COMPANION_AP_PASSWORD` 配置，默认是 `companion-display`。在浏览器打开 `http://192.168.4.1/`，填写目标 Wi-Fi 的 SSID 和密码。设备获取 DHCP 地址后，屏幕会显示 IP 地址和六位配对码。

默认端口为 HTTP/WebSocket `80` 和 UDP 视频 `5000`，可在 `menuconfig` 的 `Companion Display Wi-Fi` 中修改；该菜单还提供无帧超时（默认 10 秒）。`menuconfig` 的 `Companion Display` 菜单还提供 JPEG 帧大小上限（默认 100 KiB，同时决定协商时报告的 `max_frame_bytes`）、PSRAM 帧缓冲数量和绘制任务优先级。当前不提供 mDNS 发现，请在 PC 客户端中直接输入设备 IP。

设备主界面按信息卡片布局显示 Wi-Fi 状态、IP 地址和六位配对码，底部状态栏显示当前传输状态。视频流结束时设备回到主界面：PC 点击 Stop 或控制连接断开（含异常断开）立即返回；流启动后连续 10 秒（可配置）未收到任何完整画面时，设备主动结束会话、关闭控制连接并返回主界面，状态栏显示 `STREAM TIMEOUT`。

## PC 客户端

桌面发送端位于 `tools/desktop_client`，依赖 Rust、Node.js 和 Tauri 2。首次在 Ubuntu/Debian 上使用时，先安装系统依赖：

```bash
sudo apt install build-essential pkg-config curl clang libclang-dev \
  libwebkit2gtk-4.1-dev libgtk-3-dev libpipewire-0.3-dev \
  libssl-dev libayatana-appindicator3-dev librsvg2-dev
```

`clang` 和 `libclang-dev` 供 `src-tauri/vendor` 内 PipeWire 绑定的 bindgen 在构建期生成 C 绑定使用；缺失时 `cargo` 构建会报 `Unable to find libclang`。

安装 Node.js、npm 和 Rust stable 后，在 `companion_display` 工程目录执行：

```bash
cd tools/desktop_client
npm ci
npm run tauri:dev
```

`npm ci` 必须至少成功执行一次，否则运行时会出现 `tauri: not found`。开发窗口启动后按以下顺序完成首次联调：

1. 给设备烧录与客户端同一版本的 `companion_display` 固件。
2. 连接设备创建的 `Companion-XXXX` 热点，在浏览器访问 `http://192.168.4.1/`，让设备接入 PC 所在局域网。
3. 按设备屏幕显示的局域网 IP 和六位配对码填写客户端的 `Address` 与 `Pairing code`。填写完成约 1 秒后客户端自动检查设备可达性，也可点击 `Check` 立即检查；地址、配对码和流参数会保存在本机，下次启动自动恢复并自动检查。
4. 确认状态为可连接后，首次联调选择 TCP 和 `Static color bars`，再点击 `Start Wi-Fi Stream` 验证网络、JPEG 和显示链路。Stream 面板下方的预览图显示最近发送到设备的画面，应与设备屏幕一致。
5. 基础链路正常后点击 `Stop`，选择真实屏幕来源。Linux 选择 `Portal`，在系统弹窗中选择要共享的屏幕或窗口；实时画面推荐 UDP、25 FPS、JPEG 质量 70。

流运行期间可以直接修改 FPS 和 JPEG 质量，约 0.3 秒后生效，无需停止再启动；地址、配对码、采集来源和传输方式在流运行期间锁定，修改它们需要先 `Stop`。Portal/PipeWire 的采集帧率在启动流时协商，流中将 FPS 调高到超过启动值时，实际帧率可能受采集端限制，重新启动流即可按新值协商。

关闭开发窗口会停止客户端。后续仍在同一目录执行 `npm run tauri:dev` 即可启动。需要生成发布包时执行：

```bash
npm run tauri:build
```

构建产物位于 `src-tauri/target/release/`，安装包位于其 `bundle/` 子目录。`Static color bars` 不依赖屏幕采集权限，适合优先排除 Portal/PipeWire 权限或桌面环境问题。

Linux 客户端会在 WebKitGTK 初始化前自动设置 `WEBKIT_DISABLE_DMABUF_RENDERER=1`。这用于规避 Wayland 下部分虚拟 GPU 或显卡驱动的 DMA-BUF 合成兼容问题；该问题会使客户端窗口出现固定颜色杂块。此设置只影响客户端界面渲染，不影响 Portal/PipeWire 采集、JPEG 编码或视频传输。

客户端输出固定为 `368x448` JPEG，最多 30 FPS，JPEG 负载最多 100 KiB。默认使用 25 FPS、JPEG 质量 70；质量可在 30 到 95 之间调整，单帧编码结果超过 100 KiB 时客户端会按 5 为步长自动降低该帧质量重新编码，直到装入限制，降到 30 仍超限才丢弃该帧。采集画面会等比例缩放到设备可容纳的最大尺寸，不拉伸、不裁剪，并只在宽高比不一致时居中补齐必要黑边。Linux 的真实画面采集统一通过系统 Portal/PipeWire 对话框选择屏幕或窗口，不使用逐帧 XCap 抓屏；选择单个窗口时，客户端优先请求标准 `SPA_META_VideoCrop` 有效区域，没有该元数据时再尝试 BGRA/RGBA Alpha，从 GNOME Portal 的透明全屏画布中识别真正的窗口主体。窗口内部的黑色内容不会被误裁，手动 Crop 坐标相对于识别后的窗口区域。Portal 会将共享区域内的鼠标光标嵌入视频帧，随后与画面一起裁剪和缩放，光标在裁剪区域外时不会显示。PipeWire 回调直接生成设备尺寸图像，避免复制完整桌面；需要支持嵌入式光标的 `xdg-desktop-portal` 与桌面后端，否则客户端会拒绝启动共享。状态区以独立指标格显示传输方式、FPS、采集等待、编码、设备耗时、替换与丢弃帧数；任一耗时超过 40 ms 时对应指标变红并显示 `Performance limited`。TCP 模式一次只允许一帧在途，避免可靠传输累积旧画面。

部分 GNOME Portal 后端即使选择了单个窗口，仍会报告全屏大小的 Portal/PipeWire 外层画布。客户端会读取每帧的 `SPA_META_VideoCrop`，只处理其中声明的窗口有效区域。正常日志类似：

```text
Portal selected: type=Window logical_size=1738x3600 node=38
PipeWire negotiated: size=1738x3600 format=VideoFormat::BGRx
PipeWire VideoCrop: x=120 y=240 width=1200 height=900
Capture layout: source=1738x3600 crop=120,240,1200x900 draw=368x276 offset=0,86 target=368x448
```

其中 `logical_size` 和 `PipeWire negotiated.size` 可以仍是全屏画布；`PipeWire VideoCrop` 的 `width`、`height` 才是实际窗口区域，`Capture layout.crop` 应与其一致。客户端将该区域完整等比缩放到 `368x448`，不拉伸、不裁剪，只在宽高比不同时补最少黑边。若后端没有提供 `VideoCrop`，客户端会尝试使用 BGRA/RGBA Alpha 边界；日志出现 `Window alpha fallback: negotiated format has no alpha` 时表示两种精确边界信息都不可用，回退到完整帧后可能再次包含窗口外黑色背景。

静态画面会复用最后一次成功编码的 JPEG，以 `1 FPS` 重复发送，并为每次发送生成新帧序号；画面恢复变化后立即回到用户设置的实时帧率。这样静态内容不会占用实时带宽，也不会使设备认为视频会话失活。UDP 模式同时每 2 秒保持一次 WebSocket 控制心跳；控制连接异常时客户端会显示 `Reconnecting control channel`，最多自动重连 5 次并重新协商 UDP session，成功后继续发送当前画面。

### TCP 与 UDP 的选择

| 模式 | 视频传输 | 适用场景 |
| --- | --- | --- |
| TCP（默认） | WebSocket 二进制消息 | 首次联调、弱网络、需要完整画面时 |
| UDP | 协商后的 UDP 分片 | 稳定局域网、重视低延迟、可接受偶发丢帧时 |

两种模式都必须保持 WebSocket 控制连接。UDP 不能独立建立视频会话；设备只接受已协商会话中的视频分片。

### 低延迟验收

实时画面推荐使用 UDP、25 FPS 和 JPEG 质量 70。Linux 必须选择 `Portal` 系统对话框来源；`Static color bars` 只用于验证编码、网络和显示链路，不能代表真实桌面采集延迟。

指标格中的 `Capture` 是等待最新采集图像的时间，`Encode` 是 JPEG 编码与帧封装时间，`Device` 是设备报告的排队、解码和 LCD 提交时间。三项通常应各自低于 40 ms；任一项持续超过该值时对应指标变红并显示 `Performance limited`。`Capture` 超过 500 ms 时，优先停止流并重新建立 Portal 共享会话，而不是调整设备端参数。UDP 分片发送节奏为每毫秒最多 2 片，单帧发送预算 120 ms 不变。

实机运行时，设备状态中的 `completedFrames` 与 `submittedFrames` 应持续增长且接近；`reassemblyTimeouts` 不应持续快速增长。出现少量 UDP 丢帧是允许的，但画面不应持续落后。

## 联调与排障

- Linux 客户端窗口出现固定颜色杂块时，确认使用的是当前版本客户端。当前版本会自动禁用 WebKitGTK DMA-BUF 渲染；使用旧版本时可通过 `WEBKIT_DISABLE_DMABUF_RENDERER=1 npm run tauri:dev` 验证。该现象与 ESP32 屏幕画面和视频传输链路无关。
- 先用 TCP 和 `Static color bars` 验证基础链路。设备显示 `TCP VIDEO` 或 `RECEIVING FRAME` 即表示已进入视频接收。
- Portal 选择单个窗口后仍看到整屏黑色背景时，检查终端是否打印 `PipeWire VideoCrop`。有该日志时以其矩形为实际窗口范围；只有 `Window alpha fallback` 而没有 `PipeWire VideoCrop` 时，说明当前桌面 Portal 后端未提供可精确裁取的窗口边界。
- UDP 无画面时，查看设备串口的 `UDP stats`。`rx` 增长说明数据已到设备；`accepted` 为零时查看 `last` 拒绝原因；`submitted` 增长才表示完整帧已进入显示管线。发送端以每毫秒最多 2 片的节奏发送分片，并限制单帧发送在 120 ms 内；设备 UDP 邮箱为 24 包，持续 `timeout` 仍表示 Wi-Fi 丢包或发送端负载过高。
- `accepted` 增长而 `completed` 不增长：完整帧有分片丢失、被较新帧替换，或发送端未在预算内发完。`completed` 增长而 `submitted` 不增长：完整帧已收齐，但 `CDP1` 校验或显示管线提交失败。
- `source_ip`：UDP 来源地址与已绑定地址不一致。PC 必须将 UDP socket 绑定到 WebSocket 控制连接使用的本机 IPv4；重新启动流可创建新会话。
- `bad CRC`：完整 `CDP1` 帧的 JPEG CRC 校验失败。检查发送端和固件是否使用同一协议版本，或先改用 TCP 确认编码链路。
- `device_busy`：设备已有活动控制会话或显示管线仍被占用。先在旧客户端点击 Stop；若连接异常断开，关闭旧 WebSocket 或重启设备以释放会话。
- 点击 Stop 后客户端会显示 `stopping` 并等待 WebSocket 发送 `stop_stream`、关闭控制连接和释放设备传输；期间不要重复 Start，等状态回到 `Stopped` 后再启动。
- Stop 后再次 Start 时，部分 Portal/PipeWire 后端会先返回空帧、损坏帧或布局尚未稳定的过渡缓冲区。客户端会跳过这些帧并继续等待；终端中的 `Skipping transient PipeWire frame` 仅用于诊断。共享获批后持续 10 秒仍没有有效帧时，客户端才会报告首帧超时，此时应停止并重新选择共享对象。
- `UDP REJECTED`、`UDP DROPPED` 或 `SESSION EXPIRED`：分别表示分片被拒绝、重组超时丢帧或 5 秒未收到 UDP 数据。UDP 丢帧不重传，持续异常时改用 TCP。
- `SESSION EXPIRED` 在 UDP 模式只表示媒体暂时没有新包；设备会先保留 WebSocket 与 UDP session，静态 1 FPS 保活帧或恢复后的画面可继续使用原 session。中断持续到无帧超时（默认 10 秒）仍无完整画面时，设备结束会话、关闭控制连接并回到主界面，屏幕显示 `STREAM TIMEOUT`；PC 客户端会按控制通道重连流程自动恢复推流。Stop 或 WebSocket 关闭同样会立即释放会话并回到主界面。
- `WebSocket peer closed`：对端已显式关闭控制连接；这会释放会话。若客户端显示 `Reconnecting control channel`，它会以同一 PC IPv4 自动替换设备端遗留的旧会话，并使用新 UDP session 恢复；重连 5 次仍失败时检查 Wi-Fi、IP 与配对码。
- `PC` 或 `Device` 耗时持续接近帧间隔：降低手动 FPS 或 JPEG 质量。`Replaced` 增长说明系统正在主动跳过旧画面以维持低延迟；这比画面持续落后是预期且更适合实时显示的行为。
- Linux 真实画面延迟异常时，重新启动流并在 Portal 对话框重新选择共享对象。若 `Capture`、`Encode` 或 `Device` 任一项超过 40 ms，客户端会显示 `Performance limited`；先保持默认 25 FPS、质量 70，再依据该指标逐步降低配置。
- 本次低延迟优化使用控制协议 v2，PC 客户端和设备固件必须成对更新；客户端检查到非 v2 设备时会拒绝启动。

完整的控制消息、二进制帧和状态字段定义见 [PROTOCOL.md](PROTOCOL.md)。本版本仅适合可信局域网：配对码和 UDP 会话参数不提供传输加密。
