# Wireless Display Protocol 1.0

状态：`draft1`。本文中的“必须”“不得”“应”“可以”具有规范性含义。

## 1. 范围与术语

WDP 定义无线显示系统的发现、配对、信令、能力、会话、输入和遥测。音视频媒体使用标准 WebRTC，不定义私有 RTP、媒体分片或重传格式。

- **端点（endpoint）**：参与 WDP 会话的软件或设备。
- **发起端（initiator）**：创建会话并发送首个 SDP offer 的端点。
- **响应端（responder）**：接受或拒绝会话并返回 SDP answer 的端点。
- **资源（resource）**：显示面、媒体输入/输出、输入设备或遥测能力。
- **generation**：会话连接代次。PeerConnection 完整重建时递增。
- **revision**：能力或布局配置版本。旧 revision 的依赖消息必须丢弃。

协议版本写作 `WDP/1`。新工程不继承 Companion Display v2 的版本、消息或兼容关系。

## 2. 通用表示

- `device_id`、`client_id` 和 `session_id` 均为随机 UUIDv4；JSON 使用小写连字符文本，CBOR 使用 16 字节二进制。
- JSON 使用 UTF-8、`snake_case` 字段和十进制整数，不允许 `NaN` 或无穷值。
- DataChannel 使用 RFC 8949 CBOR，并应采用确定性编码。结构以 `protocol/wdp.cddl` 为准。
- 时间戳 `timestamp_us` 是端点启动后的单调微秒数，只用于排序和耗时计算，不代表 Unix 时间。
- 消息 ID 是发送端单调递增的无符号 64 位整数，可回绕；同一连接内不得主动重用未完成请求的 ID。
- 未知可选字段必须忽略。消息声明未知必需能力时必须返回 `unsupported_required_feature`。
- 字符串必须是合法 UTF-8。实现必须在解析前执行长度、嵌套深度和集合数量限制。

## 3. 局域网发现

可接受连接的端点必须发布 DNS-SD 服务：

```text
Service type: _wdp._tcp.local
Instance:     <用户可读设备名>
Port:         HTTPS/WSS 端口
```

TXT 记录：

| 键 | 必需 | 含义 |
| --- | --- | --- |
| `id` | 是 | 小写 UUID `device_id` |
| `ver` | 是 | 支持的主版本，draft1 固定为 `1` |
| `path` | 是 | 设备描述路径，固定 `/.well-known/wdp` |
| `pair` | 是 | draft1 固定为 `qr` |
| `name` | 否 | UTF-8 友好名称；过长时省略，使用实例名 |

TXT 总长度应控制在 400 字节内。发现信息不构成身份认证，客户端必须通过配对二维码中的证书指纹确认端点。

### 3.1 设备描述

`GET /.well-known/wdp` 无需认证，成功返回：

```json
{
  "protocol": "WDP",
  "versions": [1],
  "device_id": "3d9d2c88-a5b4-47c1-8a19-b1865423cdad",
  "name": "Studio Display",
  "pairing": {"mode": "qr", "path": "/wdp/v1/pair"},
  "signaling": {"path": "/wdp/v1/signaling", "subprotocol": "wdp.signal.v1"},
  "certificate_sha256": "base64url-sha256",
  "max_sessions": 1,
  "capabilities_etag": "opaque-revision"
}
```

响应不得包含配对密钥、长期凭据、局域网密码、活动媒体内容或详细运行时遥测。HTTP 响应应禁止共享缓存。

## 4. 配对与身份

### 4.1 配对二维码

设备在物理可信界面生成：

```text
wdp://pair?v=1&id=<uuid>&host=<hostname>&port=<port>&fp=<base64url-sha256>&secret=<base64url-random>
```

- `fp` 是当前设备 TLS 证书 DER 的 SHA-256，使用无填充 base64url。
- `secret` 必须至少包含 128 位密码学随机数，只能成功使用一次，默认 5 分钟失效。
- 新二维码必须使上一未使用密钥失效。日志和遥测不得记录完整 URI。
- 客户端必须在发送 `secret` 前验证实际 TLS 证书指纹等于 `fp`。不匹配时必须终止，不得提供跳过选项。

### 4.2 配对接口

客户端向固定证书的 `POST /wdp/v1/pair` 发送：

```json
{
  "version": 1,
  "device_id": "3d9d2c88-a5b4-47c1-8a19-b1865423cdad",
  "secret": "base64url-random",
  "client_name": "Workstation",
  "requested_permissions": ["display_send", "audio_send", "microphone_recv", "input_recv", "telemetry_recv"]
}
```

成功返回 HTTP `201`：

```json
{
  "client_id": "e028613c-06e7-44c2-bec6-b9521eb83a4a",
  "credential": "base64url-256-bit-random",
  "granted_permissions": ["display_send", "audio_send", "input_recv", "telemetry_recv"],
  "credential_version": 1
}
```

长期凭据必须至少 256 位随机数，由客户端安全存储。设备保存凭据的单向校验值、权限、创建时间和最后使用时间。权限可被物理设备界面或已授权管理接口撤销；撤销后现有会话必须终止。重复、过期或错误的 `secret` 返回通用认证失败，不得暴露密钥状态。

## 5. WSS 信令

客户端连接 `wss://HOST:PORT/wdp/v1/signaling`，提供：

```text
Sec-WebSocket-Protocol: wdp.signal.v1
Authorization: Bearer <credential>
```

服务端必须验证固定设备身份、凭据和权限。每个 JSON 信令消息使用以下信封：

```json
{
  "version": 1,
  "type": "session.create",
  "message_id": 1,
  "reply_to": null,
  "session_id": null,
  "generation": 0,
  "payload": {}
}
```

`reply_to` 只在响应中出现。消息注册表：

| `type` | 方向 | 用途 |
| --- | --- | --- |
| `session.create` | 发起端→响应端 | 请求新会话、权限和延迟策略 |
| `session.created` | 响应端→发起端 | 分配 `session_id` 并接受创建 |
| `session.reject` | 响应端→发起端 | 拒绝创建并返回错误 |
| `sdp.offer` | 发起端→响应端 | 指定 `negotiation_id` 的 SDP offer |
| `sdp.answer` | 响应端→发起端 | 对同一 `negotiation_id` 的 answer |
| `ice.candidate` | 双向 | Trickle ICE candidate |
| `ice.end` | 双向 | 当前 negotiation 的候选收集结束 |
| `ice.restart` | 双向 | 请求 ICE restart，不增加 generation |
| `session.close` | 双向 | 正常结束及原因 |
| `ping` / `pong` | 双向 | 应用层存活和 RTT 采样 |
| `error` | 双向 | 结构化错误响应 |

信令 payload 固定字段：

| `type` | payload |
| --- | --- |
| `session.create` | `client_id`、`requested_permissions`、`latency_mode`、可选 `required_features` |
| `session.created` | `session_id`、`generation`、`granted_permissions`、`signaling_resume_ms` |
| `session.reject` | `code`、`message`、`fatal`、可选 `retry_after_ms` |
| `sdp.offer` / `sdp.answer` | `negotiation_id`、`sdp` |
| `ice.candidate` | `negotiation_id`、`candidate`、`sdp_mid`、`sdp_mline_index`、可选 `username_fragment` |
| `ice.end` | `negotiation_id` |
| `ice.restart` | `negotiation_id`、`reason` |
| `session.close` | `reason`、可选 `message` |
| `ping` / `pong` | `nonce`、`timestamp_us`；pong 原样返回 nonce |
| `error` | `code`、`message`、`fatal`、可选 `retry_after_ms` |

`session.create` 成功时响应示例：

```json
{
  "version": 1,
  "type": "session.created",
  "message_id": 2,
  "reply_to": 1,
  "session_id": "0bd1ab8d-021b-40fc-93b6-c8dc6e56645e",
  "generation": 1,
  "payload": {
    "session_id": "0bd1ab8d-021b-40fc-93b6-c8dc6e56645e",
    "generation": 1,
    "granted_permissions": ["display_send", "audio_send", "input_recv"],
    "signaling_resume_ms": 10000
  }
}
```

除 `session.create` 外，会话消息必须携带非空 `session_id`。`session.created` 的信封和 payload 中 session ID、generation 必须一致，否则视为 `malformed_message`。SDP 最大 256 KiB，单次 negotiation 的 ICE candidate 最多 128 个；超出返回 `resource_exhausted`。

发起端默认是 offerer 和 ICE controlling 端。每次协商使用递增的 `negotiation_id`；候选必须携带该 ID、`candidate`、`sdp_mid`、`sdp_mline_index` 和可选 `username_fragment`。响应端不得处理旧 negotiation 的 SDP 或候选。

同一会话只允许一个未完成 offer。需要重新协商时，发起端等待上一 answer 后再发送新 offer。网络变化优先使用 ICE restart；仅当 PeerConnection 无法恢复时才递增 generation 并完整重建。

## 6. PeerConnection 要求

- 必须启用 ICE、DTLS-SRTP、RTCP multiplexing 和 BUNDLE。
- 必须支持 Trickle ICE；局域网可只产生 host candidates，公网扩展可增加 srflx/relay candidates。
- 应支持 NACK、PLI/FIR 和标准 WebRTC 拥塞控制；不得通过 WDP 自定义消息重做 RTP 重传。
- 使用应用消息的会话必须建立 SCTP DataChannel。
- 发起端在首个 offer 前创建两个 in-band DataChannel：

| Label | 参数 | 用途 |
| --- | --- | --- |
| `wdp.control.v1` | `ordered=true`，可靠 | 状态、能力、权限和不可丢输入 |
| `wdp.realtime.v1` | `ordered=false`，`maxRetransmits=0` | 触摸、指针和高频遥测 |

两个 channel 的 `protocol` 均为 `wdp.cbor.v1`。响应端必须按 label 匹配，未知 channel 应关闭。端点必须声明各 channel 最大消息长度；draft1 最低互操作值为 control 4096 字节、realtime 1200 字节。SCTP 负责传输分片，WDP 应用不得自行拆分一条消息。

## 7. CBOR 信封与消息注册表

所有 DataChannel 消息使用整数键：

| 键 | 字段 | 含义 |
| --- | --- | --- |
| `0` | `version` | 固定为 `1` |
| `1` | `type` | 下表的无符号整数 |
| `2` | `message_id` | 发送端消息 ID |
| `3` | `session_id` | 16 字节 UUID |
| `4` | `timestamp_us` | 发送端单调时间 |
| `5` | `reply_to` | 可选，被响应消息 ID |
| `6` | `flags` | 可选；bit 0 表示包含未知必需特性 |
| `7` | `payload` | 类型对应负载 |

control 类型：

| 值 | 名称 |
| --- | --- |
| 1 / 2 | `hello` / `hello_ack` |
| 3 | `capabilities` |
| 4 / 5 / 6 / 7 | `session_start` / `session_accept` / `session_reject` / `session_stop` |
| 8 / 9 | `permission_request` / `permission_result` |
| 10 | `track_manifest` |
| 11 | `surface_config` |
| 12 | `input_key` |
| 13 | `input_text` |
| 14 | `device_status` |
| 15 | `telemetry` |
| 16 | `error` |
| 17 / 18 | `clock_request` / `clock_response` |

realtime 类型：

| 值 | 名称 |
| --- | --- |
| 32 | `touch_snapshot` |
| 33 | `pointer_state` |
| 34 | `realtime_telemetry` |

DataChannel 打开后双方先发送 `hello`，确认版本、generation 和消息长度，再发送完整 `capabilities`。完成能力交换前不得发送输入或应用级会话启动消息。

### 7.1 数值注册表

媒体轨道 kind：

| 值 | 名称 |
| --- | --- |
| 1 | `display-video` |
| 2 | `playback-audio` |
| 3 | `microphone-audio` |
| 4 | `camera-video` |

权限：

| 值 | 名称 | 授权含义 |
| --- | --- | --- |
| 1 / 2 | `display_send` / `display_recv` | 发送或接收显示视频 |
| 3 / 4 | `audio_send` / `audio_recv` | 发送或接收播放音频 |
| 5 / 6 | `microphone_send` / `microphone_recv` | 发送或接收麦克风音频 |
| 7 / 8 | `input_send` / `input_recv` | 发送或注入触摸、键鼠和文字 |
| 9 | `telemetry_recv` | 接收对端设备遥测 |

其他枚举：

| 字段 | 值 |
| --- | --- |
| direction | `0=inactive`、`1=send`、`2=recv`、`3=sendrecv` |
| latency mode | `1=interactive`、`2=balanced`、`3=quality` |
| scaling | `1=contain`、`2=cover`、`3=stretch` |
| color space | `1=sRGB`、`2=BT.709`、`3=Display P3` |
| input kind | `1=touch`、`2=pointer`、`3=keyboard`、`4=consumer`、`5=text`、`6=IME` |
| track state | `0=inactive`、`1=starting`、`2=active`、`3=degraded`、`4=failed`、`5=stopping` |

## 8. 能力模型

能力负载包含：

- `revision`：无符号 32 位版本，变化时递增。
- `max_sessions` 和 control/realtime 最大消息长度。
- `surfaces`：ID、逻辑宽高、旋转、最大像素率、最大帧率和颜色空间。
- `media`：轨道种类、方向、MIME codec、fmtp、最大码率和视频/音频限制。
- `inputs`：touch、pointer、keyboard、consumer、text 和 IME 支持。
- `telemetry`：允许发送的指标集合及最大频率。
- `features`：额外可选特性；必需特性使用独立 `required_features` 数组。

方向编码：`0=inactive`、`1=send`、`2=recv`、`3=sendrecv`。能力匹配必须同时满足权限、资源方向、SDP 编解码交集和平台资源限制。没有交集时返回 `incompatible_capabilities`，不得静默选择未声明配置。

能力可以在温度、内存、设备插拔或权限变化时更新。新 revision 生效前，现有媒体可以保持旧配置；无法继续满足旧配置时，端点必须发送状态和重新协商请求，失败则安全停止相关轨道。

## 9. 应用会话与轨道

WebRTC 连接建立不等同于允许投屏。发起端发送 `session_start`，列出请求权限、延迟策略、目标 surface 和轨道意图；响应端返回 `session_accept` 或 `session_reject`。

`track_manifest` 将 SDP MID 映射到语义资源：

| kind | 用途 |
| --- | --- |
| `display-video` | 屏幕、窗口或应用渲染画面 |
| `playback-audio` | 与投屏关联的系统或媒体音频 |
| `microphone-audio` | 麦克风上传 |
| `camera-video` | 摄像头视频 |

每项必须包含 MID、kind、资源 ID、方向和可选 surface ID。MID 未出现在有效 manifest 中时，接收端不得把轨道自动绑定到物理输出。

`surface_config` 定义选中 surface、`revision`、逻辑宽高、旋转、内容矩形和缩放模式。缩放模式为 `contain`、`cover` 或 `stretch`；默认 `contain`。输入消息必须携带对应 surface ID 和 revision。

## 10. 媒体互操作基线

### 10.1 视频

- WDP 视频接收能力档必须至少支持 WebRTC H.264 Constrained Baseline、`packetization-mode=1`。
- 端点通过 SDP `profile-level-id` 和能力中的尺寸、帧率、像素率与码率声明真实上限，发送端不得超出。
- 屏幕内容应设置相应 content hint，并在 `interactive` 模式优先降低积压和帧率，而不是累计旧帧。
- MJPEG、VP8/VP9、H.265 和 AV1 均为可选；没有共同 codec 时会话拒绝。

### 10.2 音频

- WDP 音频能力档必须至少支持 Opus 48 kHz，最少单声道。
- G.711 A-law/μ-law 是资源受限端点的可选能力。
- `playback-audio` 与 `microphone-audio` 独立授权和协商，可以单向或双向启用。
- 回声消除、自动增益和降噪属于端点能力，不得假定所有嵌入式设备可用。

### 10.3 延迟策略

- `interactive`：优先低排队和输入响应，允许主动降帧、降分辨率或丢弃未展示视频。
- `balanced`：默认策略，在流畅度与清晰度之间平衡。
- `quality`：优先视觉质量，仍不得形成无界队列。

WebRTC stats 是网络与 RTP 指标的权威来源；WDP telemetry 只补充平台无法从 WebRTC 获得的采集、解码、渲染、温度和资源压力。

## 11. 输入协议

### 11.1 触摸

`touch_snapshot` 是某一 surface 当前全部活动接触点的完整快照，包含 `sequence`、surface ID、布局 revision 和 contacts。坐标、压力及接触尺寸使用 `0..65535` 归一化整数，避免绑定物理分辨率。

- 初次触摸、活动集合变化和移动时立即发送。
- 存在活动接触点时，至少每 100 ms 重发完整快照。
- 最后一个接触点释放、会话停止或 channel 关闭前必须发送空 contacts 快照。
- 接收端使用 32 位序号串行比较丢弃旧快照；空快照释放该会话的全部接触点。
- 超过 300 ms 未收到新快照时，接收端必须合成 cancel，避免永久按下。

### 11.2 指针与按键

`pointer_state` 通过 realtime channel 发送绝对或相对位置、按钮位图和滚轮状态；快照丢失后以下一状态为准。`input_key` 通过 control channel 发送 USB HID usage page、usage、按下/释放和修饰键，确保按键边沿可靠有序。

### 11.3 文字与 IME

`input_text` 使用 UTF-8，支持 `commit`、`composition_start`、`composition_update`、`composition_end` 和 `cancel`。单条文本最大 4096 字节。文字输入权限与原始键盘权限独立；接收端不得同时把同一操作作为 HID 键和已提交文本重复注入。

## 12. 状态、遥测与时钟

`device_status` 在状态变化时发送，并在活动会话中最多每 5 秒发送一次摘要，包括电源、网络、权限、活动 surface、媒体轨道和告警。

`telemetry` 通过 control channel 发送低频可靠指标；`realtime_telemetry` 最大 10 Hz，允许丢失。指标使用明确单位，例如 `temperature_millicelsius`、`free_memory_bytes`、`decode_queue_frames`、`render_latency_us` 和 `audio_underruns`。未知指标必须忽略。

draft1 遥测指标注册表：

| ID | 名称/单位 |
| --- | --- |
| 1 | `battery_level_permille`，`0..1000` |
| 2 | `temperature_millicelsius` |
| 3 | `cpu_load_permille`，`0..1000` |
| 4 / 5 | `free_memory_bytes` / `minimum_free_memory_bytes` |
| 6 | `network_rssi_dbm` |
| 7 | `available_bitrate_bps` |
| 8 / 9 | `encode_time_us` / `decode_time_us` |
| 10 / 11 | `render_latency_us` / `capture_latency_us` |
| 12 | `decode_queue_frames` |
| 13 | `dropped_video_frames`，本会话累计 |
| 14 / 15 | `audio_underruns` / `audio_overruns`，本会话累计 |
| 16 | `round_trip_time_us` |
| 17 | `packet_loss_ppm`，`0..1000000` |
| 18 | `jitter_us` |

告警 ID：`1=thermal_limit`、`2=low_memory`、`3=network_degraded`、`4=decoder_overloaded`、`5=audio_underrun`、`6=permission_revoked`、`7=battery_low`。实现可以增加厂商遥测，但 ID 必须位于 `0x80000000..0xffffffff` 且在能力扩展中声明。

`clock_request/clock_response` 使用四时间戳估计端点单调时钟偏差和 RTT，只用于把应用遥测放到近似同一时间轴；媒体同步仍由 RTP/RTCP 完成。

## 13. 关闭、恢复与清理

- 正常关闭发送 `session_stop` 和信令 `session.close`，随后关闭轨道、DataChannel 和 PeerConnection。
- WSS 断开不立即表示媒体断开，但超过协商的信令恢复窗口后必须停止会话。
- ICE disconnected 时允许有限重试和 ICE restart；默认最大重试 5 次，退避 1、2、4、8、10 秒。
- generation 变化后双方必须重新发送 hello、能力、权限、manifest、surface 配置和空输入状态。
- 任何异常结束都必须停止麦克风/摄像头、清除输入状态、释放显示与音频输出，不得保留后台采集。

## 14. 错误码

错误同时包含稳定数值 `code`、可读 `message`、`fatal` 和可选 `retry_after_ms`。不得依赖 message 做程序判断。

| code | 名称 | 含义 |
| --- | --- | --- |
| 100 | `malformed_message` | 编码、类型或字段非法 |
| 101 | `unsupported_version` | 不支持协议主版本 |
| 102 | `unauthorized` | 凭据无效或已撤销 |
| 103 | `permission_denied` | 未授予请求能力 |
| 104 | `device_busy` | 会话或资源达到上限 |
| 105 | `incompatible_capabilities` | 无可用资源或媒体交集 |
| 106 | `invalid_state` | 当前状态不允许此消息 |
| 107 | `stale_generation` | 消息属于旧连接代次 |
| 108 | `resource_exhausted` | 内存、队列或平台资源不足 |
| 109 | `media_failure` | 采集、编解码或渲染失败 |
| 110 | `timeout` | 操作超过规范时限 |
| 111 | `unsupported_required_feature` | 不识别发送方声明的必需特性 |
| 112 | `internal_error` | 未分类内部错误 |

解析错误、认证失败、generation 错误或安全策略错误通常为 fatal。资源繁忙和临时媒体失败可以给出有限重试时间。

## 15. 安全与隐私要求

- 不允许 `ws://`、明文 HTTP 信令或未加密 RTP；开发构建也必须显式开启不安全模式并在 UI 持续提示。
- 二维码密钥、长期凭据和 bearer header 不得写入普通日志、崩溃报告或遥测。
- 设备应限制配对尝试频率，连续失败后延迟并重新生成二维码。
- 客户端必须向用户显示正在使用的麦克风、摄像头和输入注入权限，并提供会话级停止入口。
- 外部信令服务只能访问路由所需的账户和会话元数据；端到端媒体及 DataChannel 内容保持 DTLS 加密。
- 实现应对 JSON、SDP 和 CBOR 解析器进行模糊测试，并限制 SDP 大小、ICE candidate 数量、CBOR 深度和数组长度。

## 16. 版本与扩展

主版本不兼容时不得连接。新增可选字段、可选 codec、遥测指标和消息类型可以保持主版本；改变既有字段语义、权限边界、DataChannel 参数或安全要求必须升级主版本。

扩展使用反向域名字符串标识，例如 `com.example.pen-v1`。双方只有在能力交集中确认扩展后才能发送对应消息。`required_features` 中存在未知值时必须拒绝会话，普通 `features` 中未知值必须忽略。
