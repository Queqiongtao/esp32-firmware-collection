# Companion Display 通信协议 v2

## 总览

协议 v2 使用 WebSocket/TCP 作为整个流生命周期内的可靠控制通道。视频可选：TCP 模式将完整帧作为 WebSocket 二进制消息发送；UDP 模式通过协商后的 UDP 端口发送同一完整帧的分片。设备同时只支持一个控制会话和一个视频流。v2 与 v1 控制协商不兼容，PC 客户端和固件必须同步升级。

默认 HTTP/WebSocket 端口为 `80`，UDP 端口为 `5000`。所有多字节二进制字段均为小端序。当前只支持 IPv4 UDP 视频。

## 连接与控制消息

客户端先连接：

```text
ws://DEVICE_IP/stream?token=PAIRING_CODE
```

`PAIRING_CODE` 必须是设备屏幕显示的六位数字配对码。未授权连接返回 HTTP `401`；已有其他活动会话时返回 HTTP `403`。控制消息是 UTF-8 JSON WebSocket 文本帧，每个请求必须包含字符串 `type` 和字符串 `request_id`。

### 状态机

```text
已连接 -> hello -> hello_ack -> start_stream -> stream_started -> 发送视频
                                                     |
                                                stop_stream
                                                     v
                                               stream_stopped
```

视频只能在收到 `stream_started` 后发送。客户端应在关闭连接前发送 `stop_stream`；WebSocket `CLOSE` 同样会释放会话和显示管线。未协商即开始、请求不属于当前会话、协议版本或传输方式不支持时，设备以 `error` 响应。

### `hello`

客户端请求：

```json
{"type":"hello","request_id":"hello-1","protocol_version":2,"video_transport":"tcp"}
```

字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `protocol_version` | 整数 | 必须为 `2` |
| `video_transport` | 字符串 | `tcp` 或 `udp` |

设备成功响应为 `hello_ack`：

```json
{"type":"hello_ack","request_id":"hello-1","protocol_version":2,"video_transport":"udp","width":368,"height":448,"max_frame_bytes":102424,"udp":{"port":5000,"session_id":1,"session_token":2,"max_payload":1200},"capabilities":{"video":["tcp","udp"],"frame_feedback":true,"touch":true,"audio_capture":true,"text_input":true}}
```

`width`、`height` 是目标画面尺寸；`max_frame_bytes` 包含 24 字节 `CDP1` 帧头；UDP 模式才返回 `udp` 对象。`session_id` 和 `session_token` 均为设备为本次会话生成的 32 位随机值，必须逐分片原样携带。`frame_feedback: true` 表示 TCP 视频会收到 `frame_presented` 回执。`capabilities` 中的 `touch`、`text_input`、`audio_capture` 仅为未来能力预留，v2 没有对应可用消息，也不启用触摸注入或麦克风采集。

### `start_stream` 与 `stop_stream`

```json
{"type":"start_stream","request_id":"start-1"}
{"type":"stop_stream","request_id":"stop-1"}
```

成功响应分别为：

```json
{"type":"stream_started","request_id":"start-1","video_transport":"udp"}
{"type":"stream_stopped","request_id":"stop-1"}
```

`start_stream` 必须在成功的 `hello` 后执行。设备在响应 `stream_started` 后开始接受所选视频通道的数据。

### 心跳与错误

UDP 视频运行期间，PC 必须每 2 秒发送一个应用层心跳：

```json
{"type":"ping","request_id":"ping-1"}
```

设备返回 `{"type":"pong","request_id":"ping-1"}`。PC 使用非阻塞方式接收 `pong`；连续 3 个心跳周期未确认时，判定控制通道失效，暂停 UDP 视频并重新建连、重新执行 `hello` 和 `start_stream`。重连成功后必须改用新的 UDP `session_id` 与 `session_token`，并继续发送最新缓存帧。WebSocket 协议层 PING/PONG 也会被处理。

控制错误统一为：

```json
{"type":"error","request_id":"...","code":"..."}
```

| `code` | 含义 |
| --- | --- |
| `unsupported_protocol` | 版本不是 2，或视频传输方式不是 `tcp`/`udp` |
| `device_busy` | 另一个会话或其他传输已占用显示管线 |
| `not_negotiated` | 未成功协商即请求启动 |
| `invalid_control_message` | 缺少 `type` 或 `request_id` |
| `unsupported_message` | v2 不支持的控制消息 |
| `tcp_video_not_started` | 在 TCP 模式未启动时发送二进制视频 |

## `CDP1` 完整视频帧

TCP 模式中，一个 WebSocket 二进制消息恰好是一帧完整 `CDP1`。UDP 模式中，`CDU1` 分片负载拼接后必须恰好恢复为同一个 `CDP1` 字节序列。

帧头固定为 24 字节：

| 偏移 | 字段 | 类型 | 值/含义 |
| --- | --- | --- | --- |
| 0 | `magic` | u32 | `0x31504443`，线上的字节为 `CDP1` |
| 4 | `version` | u8 | `1` |
| 5 | `format` | u8 | `1`，JPEG |
| 6 | `header_size` | u16 | `24` |
| 8 | `sequence` | u32 | 发送端帧序号，可回绕 |
| 12 | `width` | u16 | 目标宽度，当前为 `368` |
| 14 | `height` | u16 | 目标高度，当前为 `448` |
| 16 | `payload_size` | u32 | JPEG 字节数，范围为 `1..=102400` |
| 20 | `crc32` | u32 | JPEG payload 的 CRC-32 |

帧头之后紧跟 `payload_size` 字节 JPEG 数据。总帧大小必须严格为 `24 + payload_size`。CRC 使用与 Rust `crc32fast::hash()` 和 ESP-IDF `esp_crc32_le(0, payload, payload_size)` 相同的 CRC-32 值，只覆盖 JPEG payload，不覆盖帧头。

## `CDU1` UDP 视频分片

每个 UDP 数据报由 36 字节 `CDU1` 头和最多 1200 字节负载组成：

| 偏移 | 字段 | 类型 | 含义 |
| --- | --- | --- | --- |
| 0 | `magic` | u32 | `0x31554443`，线上的字节为 `CDU1` |
| 4 | `version` | u8 | `1` |
| 5 | `stream_type` | u8 | `1`，视频 |
| 6 | `header_size` | u16 | `36` |
| 8 | `session_id` | u32 | `hello_ack.udp.session_id` |
| 12 | `session_token` | u32 | `hello_ack.udp.session_token` |
| 16 | `frame_sequence` | u32 | 当前完整帧的序号 |
| 20 | `frame_length` | u32 | 完整 `CDP1` 的总字节数 |
| 24 | `fragment_index` | u16 | 从 0 开始的分片索引 |
| 26 | `fragment_count` | u16 | 当前帧分片总数，范围 `1..=128` |
| 28 | `fragment_offset` | u32 | 负载在完整帧中的字节偏移 |
| 32 | `payload_length` | u16 | 当前 UDP 负载长度，必须等于数据报中的实际负载且不超过 `1200` |
| 34 | `reserved` | u16 | 保留字段，当前接收端忽略 |

发送端按完整 `CDP1` 字节序列顺序切分，`fragment_offset` 为分片序号乘以协商的 `max_payload`。桌面客户端以 1 ms 间隔发送连续分片，并将单帧发送预算限制为 120 ms，避免突发包压垮设备 UDP 接收邮箱；超过预算时 PC 将该帧发送视为失败并停止本次流，不会继续积压旧画面。设备的 UDP 接收邮箱为 24 包；它校验头、会话、索引、长度和来源地址后重组。它只保留一帧重组缓冲：新帧到来时旧的不完整帧会被替换；250 ms 未收齐则丢弃；无重传和乱序恢复保证。完整帧仍必须通过 `CDP1` CRC 校验。

设备首先尝试从 TCP 控制连接取得对端 IPv4，并将 UDP 来源限制为该地址。某些 ESP-IDF WebSocket 场景下该地址可能不可用或为 `0.0.0.0`；此时设备会接受第一个已通过 `CDU1` 头、会话 ID、会话 token、分片和长度校验的 UDP 包，并将其来源 IPv4 绑定为本会话地址。后续来自其他 IPv4 的 UDP 包会以 `source_ip` 拒绝。PC 客户端必须将 UDP socket 绑定到控制连接的本机 IPv4。

接收 UDP 数据 5 秒中断时，设备显示 `SESSION EXPIRED`；当前实现保留控制会话，客户端应发送 `stop_stream` 或关闭 WebSocket 释放它。

画面静止或 Portal 暂时未提供新图像时，PC 缓存最后一个成功编码的 JPEG，并以 1 FPS 重复发送。每个保活帧使用新的 `CDP1.sequence` 和对应新的 `CDU1.frame_sequence`，JPEG payload 与 CRC 保持不变；画面变化后立即恢复用户设置的实时 FPS。设备将静态保活帧视为普通视频帧。`SESSION EXPIRED` 仅表示 UDP 媒体暂停，设备保留控制会话、UDP session/token 和来源 IP；收到同一认证 session 的下一包时恢复接收。只有 `stop_stream` 或 WebSocket 关闭释放会话。

控制连接意外断开时，PC 最多重连 5 次，退避为 1、2、4、5、5 秒。设备仅允许携带有效配对码且 TCP 来源 IPv4 与当前会话相同的新 WebSocket 替换旧会话；来自不同 IPv4 的连接仍得到 `device_busy`。替换会释放旧显示管线占用，并为新会话生成新的 UDP 参数。

## 最新帧策略与 `frame_presented`

设备显示管线只有一个待绘制槽位。新完整帧到达时会替换仍在等待绘制的旧帧；正在 JPEG 解码或已提交 LCD DMA 的帧不会被中断。该策略用于限制设备端等待时间，替换数量通过 `replaced_frames` 报告。

TCP 模式在每帧 JPEG 解码完成并提交 LCD DMA 后，设备主动发送以下 WebSocket 文本消息。PC 必须等待与已发送帧相同 `sequence` 的回执后再发送下一帧，因此可靠传输不会在内核或设备队列中堆积旧画面。

```json
{"type":"frame_presented","sequence":42,"queue_wait_us":1000,"jpeg_decode_us":18000,"lcd_wait_us":2000,"lcd_submit_us":100,"previous_dma_us":15000,"replaced_frames":3}
```

| 字段 | 含义 |
| --- | --- |
| `queue_wait_us` | 帧进入设备显示管线到开始 JPEG 解码前的排队时间 |
| `jpeg_decode_us` | JPEG 解码耗时 |
| `lcd_wait_us` | 等待上一帧 LCD DMA 完成的耗时 |
| `lcd_submit_us` | 提交当前帧 LCD DMA 的耗时 |
| `previous_dma_us` | 上一帧 LCD DMA 从提交到完成的耗时 |
| `replaced_frames` | 当前会话中被较新帧替换的待绘制帧数 |

UDP 不发送逐帧回执，避免为低延迟视频引入可靠反馈等待；PC 按手动 FPS 发送最新编码帧，并通过 `/api/status` 轮询设备诊断。Linux PC 客户端通过 Portal/PipeWire 持续接收画面，在回调中直接裁剪并双线性缩放为设备尺寸，采集槽只保留最新图像；默认发送配置为 25 FPS、JPEG 质量 70。

## HTTP 状态与诊断

`GET /api/status` 无需配对码，返回设备可用性和诊断信息：

```json
{"name":"companion-display","width":368,"height":448,"wifi":"connected","ipv4":"192.168.1.42","active_transport":"udp","token_set":true,"protocol_version":2,"video_transports":["tcp","udp"],"touch":true,"audio_capture":true,"dropped_frames":0,"performance":{"sequence":42,"queueWaitUs":1000,"jpegDecodeUs":18000,"lcdWaitUs":2000,"lcdSubmitUs":100,"previousDmaUs":15000,"replacedFrames":3},"udp":{"rxPackets":10,"acceptedFragments":10,"rejectedPackets":0,"completedFrames":1,"submittedFrames":1,"submitFailures":0,"reassemblyTimeouts":0,"lastReject":"none"}}
```

| 字段 | 含义 |
| --- | --- |
| `wifi` | `setup_required`、`connecting` 或 `connected` |
| `active_transport` | `none`、`tcp` 或 `udp` |
| `protocol_version` | 当前控制协议版本 |
| `video_transports` | 设备支持的视频传输方式 |
| `dropped_frames` | 设备显示管线报告的丢帧计数 |
| `performance` | 最近一帧的设备耗时和 `replacedFrames`；字段使用 camelCase |
| `udp.rxPackets` | 收到且长度至少为 UDP 头的包数 |
| `udp.acceptedFragments` | 通过校验并参与重组的分片数 |
| `udp.rejectedPackets` | 被拒绝的 UDP 包数 |
| `udp.completedFrames` | 分片收齐的完整帧数 |
| `udp.submittedFrames` | 通过 `CDP1` 校验并提交显示管线的帧数 |
| `udp.submitFailures` | 已收齐但帧头、CRC、缓冲或管线检查失败的帧数 |
| `udp.reassemblyTimeouts` | 重组超时或被新帧替换的次数 |
| `udp.lastReject` | 最近拒绝原因：`none`、`inactive`、`source_ip`、`header`、`session`、`fragment` 或 `length` |

UDP 处理链路为 `rxPackets -> acceptedFragments -> completedFrames -> submittedFrames`。`acceptedFragments` 增长而 `completedFrames` 不增长表示分片未收齐；`completedFrames` 增长而 `submittedFrames` 不增长表示完整帧未通过 `CDP1` 校验或无法提交显示管线。`reassemblyTimeouts` 统计 250 ms 超时以及被较新帧替换的未完成重组，少量增长是 UDP 的预期行为，持续增长表示链路或发送节奏不稳定。

设备屏幕和串口同时提供状态。关键屏幕状态包括 `WS CONNECTED`、`NEGOTIATING`、`TCP VIDEO`、`UDP VIDEO`、`RECEIVING FRAME`、`BAD CRC`、`UDP REJECTED`、`UDP DROPPED`、`SESSION EXPIRED` 与 `DEVICE BUSY`。UDP 活动期间，串口每秒输出一次 `UDP stats: rx=... accepted=... rejected=... completed=... submitted=... timeout=... failed=... last=...`。

## 安全与兼容性

本版本面向可信局域网。六位配对码保护 WebSocket 建连；UDP session 值和来源 IPv4 绑定用于降低误收和无关数据报风险，但不提供加密、身份认证强度保证或防重放保护。DTLS/TLS 尚未实现。

未来的触摸、文本输入和音频能力必须继续经可靠控制通道协商，并在新协议版本中明确消息、权限、数据方向、生命周期和兼容策略。不得复用 v2 未定义的消息而改变既有客户端行为。
