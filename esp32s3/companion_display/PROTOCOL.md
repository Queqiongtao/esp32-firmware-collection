# Companion Display Protocol v1

The desktop client first connects to `ws://DEVICE/stream?token=PAIRING_CODE`. The
WebSocket is the reliable control channel for the complete lifetime of a stream.

## Control channel

Control messages are UTF-8 JSON WebSocket text messages. Every request contains
`type` and `request_id`; `hello` additionally contains `protocol_version: 1`.

1. The client sends `hello` with `video_transport` set to `tcp` or `udp`.
2. The device returns `hello_ack`, confirming the selected mode and capabilities.
   UDP replies include `udp.port`, `udp.session_id`, `udp.session_token`, and
   `udp.max_payload`.
3. The client sends `start_stream`; the device returns `stream_started` before
   accepting video frames.
4. The client sends `stop_stream` before closing. A disconnected WebSocket also
   releases the active stream.

Errors use `{ "type": "error", "request_id": "...", "code": "..." }`.
Unsupported versions, modes, and out-of-order requests are rejected.

The device advertises `touch`, `text_input`, and `audio_capture` capabilities.
They reserve reliable control messages for future `touch_event`, `text_input`,
and `audio_control` features. They do not enable input injection or microphone
capture in protocol v1.

## Video payloads

TCP video uses one WebSocket binary message per existing `CDP1` frame. A `CDP1`
frame is a 24-byte little-endian header followed by the JPEG payload and its
CRC-32.

UDP video divides the same complete `CDP1` byte sequence into data grams. Each
data gram starts with this packed little-endian 36-byte header:

| Field | Type |
| --- | --- |
| magic (`CDU1`) | u32 |
| protocol version | u8 |
| stream type (`video`) | u8 |
| header size | u16 |
| session ID | u32 |
| session token | u32 |
| frame sequence | u32 |
| complete frame length | u32 |
| fragment index / count | u16 / u16 |
| byte offset | u32 |
| payload length | u16 |
| reserved | u16 |

The device accepts only the negotiated source IPv4 address, session ID, and
session token. The desktop client binds its UDP socket to the same local IPv4
address selected by the TCP control connection. Payloads are capped at 1200
bytes. A frame whose fragments are missing after 250 ms is discarded with no
retransmission; a later complete frame is preferred. The existing `CDP1` CRC
remains the end-to-end frame validation.

`/api/status` includes UDP receive, accepted-fragment, rejection, completed-frame,
submitted-frame, timeout, and last-rejection-reason diagnostics for live testing.

## Security and limits

This version is designed for a trusted LAN. Pairing authenticates the WebSocket;
the UDP session values and source-IP binding reduce accidental or unsolicited
packets but do not encrypt media. DTLS is intentionally deferred. One control
session and one video stream are supported at a time.
