# WDP 架构设计

## 1. 设计边界

WDP 将投屏系统拆成发现、身份、信令、媒体和应用数据五层。硬件差异通过能力声明表达，WebRTC 标准栈负责实时媒体，不在 WDP 中重复实现分片、重传、加密或时钟同步。

```text
应用                    会话、权限、显示布局、输入、状态、诊断
WDP DataChannel         CBOR：control（可靠）/ realtime（低延迟）
WDP 信令                HTTPS/WSS：认证、SDP、ICE、会话生命周期
WebRTC                  ICE + DTLS + SRTP/SCTP + RTP/RTCP
平台适配                libwebrtc / GStreamer / esp_peer / 系统音视频 API
```

WDP 端点可以同时拥有来源和接收能力。例如手机可以向显示器投屏，也可以作为另一台设备的触摸控制器；RK 设备可以接收桌面视频、播放伴音并上传麦克风；ESP32 可以只声明单路视频接收、单声道音频和触摸发送。

## 2. 端点与资源模型

每个端点具有稳定的 `device_id`、可变的友好名称和一组资源：

- `surface`：显示面及其逻辑尺寸、方向、像素率、帧率和颜色能力。
- `video`：摄像头或屏幕轨道的发送/接收方向及编解码能力。
- `audio_output`：扬声器或线路输出能力。
- `audio_input`：麦克风能力。
- `input`：触摸、指针、键盘、Consumer Control 和文字/IME。
- `telemetry`：电量、温度、负载、内存、网络和媒体管线指标。

能力是约束而不是配置。会话建立后，双方从能力交集和 SDP 结果生成实际配置；任何运行时变更都带 `revision`，旧 revision 的输入或布局消息必须丢弃。

## 3. 局域网连接流程

```text
接收端                 来源端
  | mDNS _wdp._tcp       |
  |<---------------------| 发现
  | 显示配对二维码       |
  |<==== 扫码/导入 ======| 固定证书指纹
  | HTTPS /pair          |
  |<---------------------| 一次性密钥
  | client credential    |
  |--------------------->|
  | WSS signaling        |
  |<====================>| SDP + Trickle ICE
  | WebRTC PeerConnection|
  |<====================>| DTLS-SRTP/SCTP
  | control/realtime     |
  |<====================>| 能力、输入、遥测
```

接收端内置信令适合离线局域网。公网场景通过相同信令消息接入外部服务，并由 ICE 使用 STUN/TURN；外部服务不得解密 DTLS-SRTP 媒体或 DataChannel 数据。

## 4. 媒体模型

媒体轨道使用标准 WebRTC `sendonly`、`recvonly` 或 `sendrecv` 方向。WDP 通过轨道清单将 SDP MID 关联到以下语义：

- `display-video`：屏幕、窗口或渲染画面。
- `playback-audio`：与投屏相关的系统音频或媒体音频。
- `microphone-audio`：端点麦克风采集。
- `camera-video`：可选摄像头轨道，不与显示轨道混用。

H.264 与 Opus 是 WDP 互操作基线。端点可以额外声明 G.711、MJPEG、VP8/VP9、H.265 或 AV1；只有 SDP 与 WDP 能力清单共同确认的组合才能启用。分辨率、帧率、声道数和码率均来自能力协商，不设全局固定值。

延迟策略分为 `interactive`、`balanced` 和 `quality`。WebRTC 根据 RTCP 与带宽估计调整码率；WDP 设备遥测补充解码队列、渲染耗时、温度和内存压力，来源端可以据此降低分辨率或帧率。

## 5. DataChannel 分工

### `wdp.control.v1`

可靠、有序。承载握手、能力、权限、会话、轨道清单、显示布局、键盘、文字/IME、设备状态、时钟采样和错误。消息量小但状态不可丢失。

### `wdp.realtime.v1`

无序，`maxRetransmits=0`。承载触摸快照、指针状态和高频遥测。实时输入采用带序号的状态快照而不是只发送边沿事件；发送端周期性重发完整状态，丢包后可以自动恢复，空触摸快照会释放全部接触点。

DataChannel 未就绪前不得注入输入或启动依赖应用控制的媒体。断线重建后，双方必须重新发送能力、权限、布局和当前输入空状态，不沿用旧 channel 的隐含状态。

## 6. 安全与权限

- 首次配对二维码提供至少 128 位一次性随机密钥和设备 TLS 证书 SHA-256 指纹。
- 客户端必须先验证指纹，再提交配对密钥；成功后获得随机、可撤销且有权限范围的客户端凭据。
- 信令只接受 TLS；媒体和 DataChannel 必须使用 WebRTC DTLS-SRTP/SCTP。
- 麦克风、摄像头、触摸/键鼠注入和文字输入分别授权，并支持设备端显示使用指示和立即撤销。
- 未配对发现接口只能暴露连接所需的最少设备信息，不得返回遥测、网络凭据或活动内容。
- 日志、错误和遥测不得包含配对密钥、长期凭据、SDP 中的敏感认证材料或媒体内容。

## 7. 会话与恢复

端点通过 `max_sessions` 声明最大并发数，默认值为 1。每个会话使用随机 128 位 `session_id` 和单调递增的 `generation`。网络切换优先执行 ICE restart；PeerConnection 必须重建时增加 generation，旧 generation 的信令和 DataChannel 消息一律忽略。

正常停止需要双方交换结束原因并关闭媒体轨道、DataChannel 和 PeerConnection。异常断开后必须清空输入状态、停止麦克风权限和释放显示资源；是否自动重连由会话策略决定，不得无限重试。

## 8. 参考核心边界

后续 Rust `wdp-core` 只实现协议状态机、能力匹配、CBOR、凭据元数据和测试向量，不绑定具体 WebRTC 库。平台通过以下适配边界接入：

- `DiscoveryBackend`：mDNS 发布与浏览。
- `CredentialStore`：系统钥匙串、Android Keystore、iOS Keychain 或设备 NVS。
- `SignalingTransport`：内置 WSS 或外部信令。
- `PeerBackend`：libwebrtc、GStreamer WebRTC、`esp_peer` 等。
- `PlatformMedia`：采集、编解码、渲染、音频设备和输入注入。

C ABI 使用不透明句柄、显式长度和调用方提供的回调；不跨 ABI 暴露 Rust 容器、字符串或异步运行时类型。
