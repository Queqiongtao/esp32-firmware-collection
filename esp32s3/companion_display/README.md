# Companion Display

`companion_display` 将 Waveshare ESP32-S3-Touch-AMOLED-1.8 用作桌面伴侣显示屏。它通过 Wi-Fi 接收桌面、窗口或裁剪区域的 JPEG 画面；它不是操作系统的扩展显示器，也不需要安装显示驱动。

视频有两种模式：TCP 模式通过 WebSocket 可靠传输，适合首次联调与稳定网络；UDP 模式仍以 WebSocket/TCP 保持控制连接，视频分片改用 UDP，以丢弃不完整帧换取更低延迟。

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

客户端输出固定为 `368x448` JPEG，最多 30 FPS，JPEG 负载最多 100 KiB。采集画面会等比例缩放，并以黑边补齐。

### TCP 与 UDP 的选择

| 模式 | 视频传输 | 适用场景 |
| --- | --- | --- |
| TCP（默认） | WebSocket 二进制消息 | 首次联调、弱网络、需要完整画面时 |
| UDP | 协商后的 UDP 分片 | 稳定局域网、重视低延迟、可接受偶发丢帧时 |

两种模式都必须保持 WebSocket 控制连接。UDP 不能独立建立视频会话；设备只接受已协商会话中的视频分片。

## 联调与排障

- 先用 TCP 和 `Static color bars` 验证基础链路。设备显示 `TCP VIDEO` 或 `RECEIVING FRAME` 即表示已进入视频接收。
- UDP 无画面时，查看设备串口的 `UDP stats`。`rx` 增长说明数据已到设备；`accepted` 为零时查看 `last` 拒绝原因；`submitted` 增长才表示完整帧已进入显示管线。
- `source_ip`：UDP 来源地址与已绑定地址不一致。PC 必须将 UDP socket 绑定到 WebSocket 控制连接使用的本机 IPv4；重新启动流可创建新会话。
- `bad CRC`：完整 `CDP1` 帧的 JPEG CRC 校验失败。检查发送端和固件是否使用同一协议版本，或先改用 TCP 确认编码链路。
- `device_busy`：设备已有活动控制会话或显示管线仍被占用。先在旧客户端点击 Stop；若连接异常断开，关闭旧 WebSocket 或重启设备以释放会话。
- `UDP REJECTED`、`UDP DROPPED` 或 `SESSION EXPIRED`：分别表示分片被拒绝、重组超时丢帧或 5 秒未收到 UDP 数据。UDP 丢帧不重传，持续异常时改用 TCP。

完整的控制消息、二进制帧和状态字段定义见 [PROTOCOL.md](PROTOCOL.md)。本版本仅适合可信局域网：配对码和 UDP 会话参数不提供传输加密。
