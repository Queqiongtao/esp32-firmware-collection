# Companion Display ESP32-S3 固件

`companion_display` 将 Waveshare ESP32-S3-Touch-AMOLED-1.8 用作桌面伴侣显示屏。PC 客户端采集桌面、窗口或裁剪区域，将画面压缩为 JPEG 后通过局域网发送给设备。它不是操作系统扩展显示器，不需要安装虚拟显示驱动。

端到端系统由设备工程和仓库公共客户端组成：

- 设备端：ESP-IDF 固件，负责配网、鉴权、控制会话、视频接收、JPEG 解码和 AMOLED 显示。
- PC 端：位于本产品工程 `desktop_client` 的公共 Tauri 2 + Vue 3 客户端，可供后续 S3/P4 设备共同使用。

控制通道始终使用 WebSocket/TCP。视频可选择 TCP 或 UDP：TCP 模式可靠、适合首次联调；UDP 模式允许丢弃不完整帧，以换取更低延迟。两端都采用“最新画面优先”策略，处理不过来时替换旧帧，不持续累积延迟。

## 文档导航

- [设备端实现](docs/device.md)：固件构建、板级驱动、显示、帧管线、配网、控制会话和设备诊断。
- [PC 端实现](../../docs/pc-client.md)：客户端构建、界面状态、画面采集、编码、传输、重连和性能诊断。
- [通信协议 v2](../../PROTOCOL.md)：S3/P4 公共 HTTP 接口、WebSocket 控制消息、`CDP1` 完整帧和 `CDU1` UDP 分片。
- [显示颜色格式](docs/display-color-format.md)：SH8601 RGB565 字节序和颜色常量约束。

## 端到端数据流

```text
桌面/窗口
   |
   v
PC 采集 -> Crop/等比缩放/补黑边 -> JPEG 编码 -> CDP1 封装
                                                |          |
                                                | TCP      | UDP 分片 CDU1
                                                v          v
                                        WebSocket       UDP 5000
                                                \          /
                                                 设备帧管线
                                                      |
                                                      v
                                              JPEG -> RGB565 -> AMOLED
```

WebSocket 不仅用于 TCP 视频，也负责 UDP 模式的鉴权、协商、启停、心跳和会话生命周期。UDP 数据不能脱离控制连接独立建立视频会话。

## 快速开始

### 1. 构建与烧录固件

工程使用 `eim` 选择和运行 ESP-IDF 环境。在本目录执行：

```bash
./dev.sh build
./dev.sh -p /dev/ttyACM0 flash-monitor
```

请按实际设备替换串口，例如 `/dev/ttyUSB0`。可通过 `./dev.sh ports` 查看端口，通过 `./dev.sh menuconfig` 修改项目配置。

### 2. 为设备配置 Wi-Fi

1. 连接设备创建的 `Companion-XXXX` 热点。
2. 默认热点密码为 `companion-display`。
3. 浏览器打开 `http://192.168.4.1/`，填写目标 Wi-Fi 的 SSID 和密码。
4. 设备获得 DHCP 地址后，屏幕会显示局域网 IPv4 地址和六位配对码。

设备默认使用 HTTP/WebSocket 端口 `80` 和 UDP 视频端口 `5000`。当前不提供 mDNS 发现，PC 客户端需要直接填写设备 IP。

### 3. 启动 PC 客户端

Ubuntu/Debian 首次使用先安装系统依赖：

```bash
sudo apt install build-essential pkg-config curl clang libclang-dev \
  libwebkit2gtk-4.1-dev libgtk-3-dev libpipewire-0.3-dev \
  libssl-dev libayatana-appindicator3-dev librsvg2-dev
```

安装 Node.js、npm 和 Rust stable 后执行：

```bash
cd ../../desktop_client
npm ci
npm run tauri:dev
```

`npm ci` 必须至少成功执行一次，否则 Tauri CLI 不可用。生成发布包使用：

```bash
npm run tauri:build
```

客户端顶部提供中英文切换。首次启动时，系统语言以 `zh` 开头则使用中文，否则使用英文；手动选择会保存在本机并在后续启动时优先恢复。按钮、面板、常用状态、校验提示和操作日志会即时切换，PipeWire、网络和协议底层错误保留英文原文以便检索排障。

主区设备预览会根据窗口剩余空间等比例缩放，并使用设备协商尺寸计算竖屏或横屏比例；当前 S3 分别为 `368:448` 和 `448:368`。画面始终使用 contain 模式完整显示。窗口宽度小于约 1100px 时预览位于上方，参数面板和日志通过页面纵向滚动访问。默认最小窗口 `720×560` 是保证完整预览的支持下限。

### 4. 首次联调

1. 在 `Address` 中填写设备 IP。客户端会自动规范化为 `ws://DEVICE_IP/stream`。
2. 填写屏幕显示的六位 `Pairing code`，等待自动检查或点击 `Check`。
3. 选择 TCP、`Static color bars`，点击 `Start Wi-Fi Stream`。
4. 确认客户端预览和设备画面一致后点击 `Stop`。
5. Linux 默认选择 `Portal`。首次投屏在系统对话框中选择屏幕或窗口；客户端会保存 Portal restore token，桌面后端支持时后续启停和客户端重启可直接恢复。实时画面推荐 UDP、25 FPS、JPEG 质量 70。

需要只投送部分画面时可切换到“区域投屏”：选择屏幕或窗口后，在全屏冻结预览上鼠标框选；Linux 自动恢复了上次来源时，可点击“更换屏幕或窗口”立即重新打开系统选择框。确认后该区域会按设置 FPS 持续实时投送。运行中可以重新框选，取消时保持原区域；点击 Stop 后设备按既有协议返回主页。

`Display orientation` 按设备实际摆放方向提供四档：顶部朝上、顶部朝左、顶部朝下、顶部朝右。当前 S3 横放时 PC 使用 `448x368` 逻辑画布，再旋转为设备接收的 `368x448` 帧；其他设备使用各自协商宽高执行相同处理。

流运行期间可以修改 FPS 和 JPEG 质量，约 0.3 秒后生效。设备地址、配对码、采集来源、投屏模式、显示方向和传输方式在流运行期间锁定，修改前需要先停止流；区域投屏可通过“重新框选区域”更新当前选区。

需要更换已记住的共享对象时，在停止状态下展开 `Source`，点击 `Choose again`/`重新选择`，下一次 Start 会重新打开 Portal 系统选择框。Portal 是否免确认由桌面环境安全策略决定；不支持 restore token 的后端仍会按原流程弹框。

## TCP 与 UDP

| 模式 | 视频路径 | 流控方式 | 推荐场景 |
| --- | --- | --- | --- |
| TCP | WebSocket 二进制帧 | 一次只允许一帧在途，等待设备 `frame_presented` | 首次联调、弱网络、优先保证完整帧 |
| UDP | `CDU1` 分片数据报 | 不重传，设备只重组当前最新帧 | 稳定局域网、重视低延迟、允许偶发丢帧 |

两种模式都要求 PC 客户端与设备固件使用控制协议 v2。v2 与旧控制协商不兼容。

## 默认能力与限制

- 本 S3 固件协商输出尺寸为 `368x448`；公共客户端不固定设备尺寸。
- PC 端支持四向设备摆放；横屏逻辑视图为 `448x368`，线上帧仍为 `368x448`。
- PC 最大帧率：30 FPS；默认 25 FPS。
- JPEG 质量范围：30～95；默认 70。
- 本 S3 固件 JPEG payload 上限为 100 KiB；公共客户端读取 `hello_ack.max_frame_bytes`。
- 静态画面以 1 FPS 重发最后一次成功编码的 JPEG，维持设备会话活性。
- 设备默认完整帧无数据超时为 10 秒；超时后释放传输并回到主信息界面。
- 主区预览由 Rust 端限制为 1 FPS，与实际投屏帧率解耦。
- 当前协议只适合可信局域网，不提供 TLS、DTLS、强身份认证或防重放保护。
- 板级代码包含触摸、音频、SD、RTC 和 IMU 接口，但当前 v2 没有对应的远程触摸、文字输入或音频控制消息。

## 快速排障

- 先使用 TCP + `Static color bars`，将采集权限问题与网络、编码、显示问题分开。
- 配对失败时检查设备屏幕上的六位配对码；HTTP `401` 表示配对码错误，`403` 表示设备已有活动会话。
- UDP 无画面时查看设备串口 `UDP stats`；`rx`、`accepted`、`completed`、`submitted` 应依次增长。
- `Replaced` 增长表示系统主动跳过旧画面以维持低延迟，不等同于连接失败。
- `Capture`、`Encode` 或 `Device` 持续接近当前帧间隔时，降低 FPS 或 JPEG 质量。
- Linux 客户端窗口出现固定颜色杂块时确认使用当前版本；客户端会自动设置 `WEBKIT_DISABLE_DMABUF_RENDERER=1`。
- 详细状态含义和分端排障见[设备端实现](docs/device.md#诊断与排障)与[PC 端实现](../../docs/pc-client.md#性能与排障)。
