mod orientation;
mod protocol;
#[cfg(target_os = "linux")]
mod wayland_capture;

use std::io::{Cursor, Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use image::imageops::{overlay, FilterType};
use image::{DynamicImage, ImageBuffer, Rgb};
use orientation::DisplayOrientation;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, Manager, State};
use tungstenite::{client, Error as WebSocketError, HandshakeError, Message, WebSocket};
#[cfg(not(target_os = "linux"))]
use xcap::{Monitor, Window};

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct CaptureSourceInfo {
    id: String,
    kind: String,
    label: String,
    width: u32,
    height: u32,
}

#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct StreamConfig {
    endpoint: String,
    token: String,
    source_id: String,
    region_x: u32,
    region_y: u32,
    region_width: u32,
    region_height: u32,
    fps: u8,
    jpeg_quality: u8,
    video_transport: String,
    #[serde(default)]
    display_orientation: DisplayOrientation,
    #[serde(default)]
    portal_restore_token: Option<String>,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct StreamEvent {
    state: String,
    detail: String,
    fps: u8,
    jpeg_bytes: usize,
    dropped_frames: u32,
    video_transport: String,
    capture_encode_ms: u32,
    capture_wait_ms: u32,
    device_ms: u32,
    device_replaced_frames: u32,
}

impl Default for StreamEvent {
    fn default() -> Self {
        Self {
            state: "idle".into(),
            detail: "Disconnected".into(),
            fps: 0,
            jpeg_bytes: 0,
            dropped_frames: 0,
            video_transport: "none".into(),
            capture_encode_ms: 0,
            capture_wait_ms: 0,
            device_ms: 0,
            device_replaced_frames: 0,
        }
    }
}

#[derive(Clone, Deserialize, Serialize)]
struct DeviceStatus {
    name: String,
    width: u16,
    height: u16,
    wifi: String,
    ipv4: String,
    #[serde(rename = "activeTransport", alias = "active_transport")]
    active_transport: String,
    #[serde(rename = "tokenSet", alias = "token_set")]
    token_set: bool,
    #[serde(rename = "protocolVersion", alias = "protocol_version", default)]
    protocol_version: u8,
    #[serde(rename = "videoTransports", alias = "video_transports", default)]
    video_transports: Vec<String>,
    #[serde(rename = "droppedFrames", alias = "dropped_frames", default)]
    dropped_frames: u32,
    #[serde(default)]
    udp: UdpDiagnostics,
    #[serde(default)]
    performance: DevicePerformance,
}

#[derive(Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct UdpDiagnostics {
    rx_packets: u32,
    accepted_fragments: u32,
    rejected_packets: u32,
    completed_frames: u32,
    submitted_frames: u32,
    submit_failures: u32,
    reassembly_timeouts: u32,
    last_reject: String,
}

#[derive(Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct DevicePerformance {
    sequence: u32,
    queue_wait_us: u32,
    jpeg_decode_us: u32,
    lcd_wait_us: u32,
    lcd_submit_us: u32,
    previous_dma_us: u32,
    replaced_frames: u32,
}

#[derive(Deserialize)]
struct UdpParameters {
    port: u16,
    session_id: u32,
    session_token: u32,
    max_payload: usize,
}

#[derive(Deserialize)]
struct ControlReply {
    #[serde(rename = "type")]
    kind: String,
    request_id: String,
    #[serde(default)]
    code: String,
    #[serde(default)]
    video_transport: String,
    udp: Option<UdpParameters>,
}

#[derive(Deserialize)]
struct FramePresented {
    #[serde(rename = "type")]
    kind: String,
    sequence: u32,
    queue_wait_us: u32,
    jpeg_decode_us: u32,
    lcd_wait_us: u32,
    lcd_submit_us: u32,
    previous_dma_us: u32,
    replaced_frames: u32,
}

enum VideoSender {
    Tcp,
    Udp {
        socket: UdpSocket,
        parameters: UdpParameters,
        sent_packets: u64,
        sent_bytes: u64,
    },
}

/* FPS and JPEG quality can be retuned while a stream is running; the stream
thread reads them every loop iteration. */
#[derive(Default)]
struct LiveParams {
    fps: AtomicU8,
    quality: AtomicU8,
}

#[derive(Default)]
struct StreamState {
    stop: Arc<Mutex<Option<Arc<AtomicBool>>>>,
    event: Arc<Mutex<StreamEvent>>,
    live: Arc<LiveParams>,
}

enum CaptureSession {
    StaticColorBars(DynamicImage),
    #[cfg(target_os = "linux")]
    PipeWire(Arc<wayland_capture::LatestFrame>),
    #[cfg(not(target_os = "linux"))]
    Monitor(Monitor),
    #[cfg(not(target_os = "linux"))]
    Window(Mutex<Window>),
}

const CONNECTION_TIMEOUT: Duration = Duration::from_secs(5);
const CONTROL_PROTOCOL_VERSION: u8 = 2;
const STATIC_COLOR_BARS_ID: &str = "test:color-bars";
const UDP_FRAGMENT_SPACING: Duration = Duration::from_millis(1);
/* Two ~1.2 KB datagrams per tick stays far below the device's 24-packet
receive mailbox while halving per-frame pacing latency. */
const UDP_FRAGMENTS_PER_TICK: usize = 2;
const UDP_FRAME_SEND_LIMIT: Duration = Duration::from_millis(120);
const STATIC_FRAME_INTERVAL: Duration = Duration::from_secs(1);
const TCP_PRESENTED_POLL_INTERVAL: Duration = Duration::from_millis(200);
const CONTROL_HEARTBEAT_INTERVAL: Duration = Duration::from_secs(2);
const CONTROL_HEARTBEAT_MISSES: u8 = 3;
const CONTROL_RECONNECT_DELAYS: [Duration; 5] = [
    Duration::from_secs(1),
    Duration::from_secs(2),
    Duration::from_secs(4),
    Duration::from_secs(5),
    Duration::from_secs(5),
];

struct UdpControlHeartbeat {
    next_ping: Instant,
    pending_request_id: Option<String>,
    misses: u8,
    sequence: u32,
}

impl UdpControlHeartbeat {
    fn new() -> Self {
        Self {
            next_ping: Instant::now() + CONTROL_HEARTBEAT_INTERVAL,
            pending_request_id: None,
            misses: 0,
            sequence: 0,
        }
    }
}

#[cfg(not(target_os = "linux"))]
fn xcap_error(error: impl std::fmt::Display) -> String {
    format!("capture error: {error}")
}

fn list_capture_sources_impl() -> Result<Vec<CaptureSourceInfo>, String> {
    let mut sources = vec![CaptureSourceInfo {
        id: STATIC_COLOR_BARS_ID.into(),
        kind: "test".into(),
        label: "Static color bars".into(),
        width: u32::from(protocol::WIDTH),
        height: u32::from(protocol::HEIGHT),
    }];
    #[cfg(target_os = "linux")]
    {
        sources.push(CaptureSourceInfo {
            id: "linux:portal".into(),
            kind: "portal".into(),
            label: "Choose in system dialog".into(),
            width: 0,
            height: 0,
        });
        return Ok(sources);
    }
    #[cfg(not(target_os = "linux"))]
    {
        for (index, monitor) in Monitor::all().map_err(xcap_error)?.into_iter().enumerate() {
            sources.push(CaptureSourceInfo {
                id: format!("monitor:{index}"),
                kind: "monitor".into(),
                label: monitor.name().to_owned(),
                width: monitor.width(),
                height: monitor.height(),
            });
        }
        for (index, window) in Window::all().map_err(xcap_error)?.into_iter().enumerate() {
            let title = window.title().to_owned();
            if title.trim().is_empty() {
                continue;
            }
            sources.push(CaptureSourceInfo {
                id: format!("window:{index}"),
                kind: "window".into(),
                label: title,
                width: window.width(),
                height: window.height(),
            });
        }
        Ok(sources)
    }
}

fn static_color_bars(width: u32, height: u32) -> DynamicImage {
    const COLORS: [[u8; 3]; 8] = [
        [255, 255, 255],
        [255, 255, 0],
        [0, 255, 255],
        [0, 255, 0],
        [255, 0, 255],
        [255, 0, 0],
        [0, 0, 255],
        [0, 0, 0],
    ];
    let image = ImageBuffer::from_fn(width, height, |x, _| {
        Rgb(COLORS[(x as usize * COLORS.len()) / width as usize])
    });
    DynamicImage::ImageRgb8(image)
}

#[cfg(not(target_os = "linux"))]
fn open_xcap_source(source_id: &str) -> Result<CaptureSession, String> {
    let (kind, index) = source_id
        .split_once(':')
        .ok_or_else(|| "invalid capture source".to_string())?;
    let index = index
        .parse::<usize>()
        .map_err(|_| "invalid capture source index")?;
    match kind {
        "monitor" => {
            let monitor = Monitor::all()
                .map_err(xcap_error)?
                .into_iter()
                .nth(index)
                .ok_or_else(|| "selected monitor is no longer available".to_string())?;
            Ok(CaptureSession::Monitor(monitor))
        }
        "window" => {
            let window = Window::all()
                .map_err(xcap_error)?
                .into_iter()
                .nth(index)
                .ok_or_else(|| "selected window is no longer available".to_string())?;
            Ok(CaptureSession::Window(Mutex::new(window)))
        }
        _ => Err("unsupported capture source".to_string()),
    }
}

fn crop_and_letterbox(image: DynamicImage, config: &StreamConfig) -> Result<DynamicImage, String> {
    let source_width = image.width();
    let source_height = image.height();
    if source_width == 0 || source_height == 0 {
        return Err("capture source returned an empty image".to_string());
    }
    let crop_width = if config.region_width == 0 {
        source_width
    } else {
        config.region_width
    };
    let crop_height = if config.region_height == 0 {
        source_height
    } else {
        config.region_height
    };
    if config.region_x >= source_width
        || config.region_y >= source_height
        || crop_width > source_width - config.region_x
        || crop_height > source_height - config.region_y
    {
        return Err("capture region is outside the selected source".to_string());
    }
    let cropped = image.crop_imm(config.region_x, config.region_y, crop_width, crop_height);
    let physical_width = u32::from(protocol::WIDTH);
    let physical_height = u32::from(protocol::HEIGHT);
    let (logical_width, logical_height) = config
        .display_orientation
        .logical_size(physical_width, physical_height);
    let resized = cropped
        .resize(logical_width, logical_height, FilterType::Triangle)
        .to_rgb8();
    let mut canvas = ImageBuffer::from_pixel(logical_width, logical_height, Rgb([0, 0, 0]));
    let offset_x = (canvas.width() - resized.width()) / 2;
    let offset_y = (canvas.height() - resized.height()) / 2;
    overlay(
        &mut canvas,
        &resized,
        i64::from(offset_x),
        i64::from(offset_y),
    );
    Ok(DynamicImage::ImageRgb8(
        config.display_orientation.orient_logical_image(&canvas),
    ))
}

/* Returns the encoded JPEG and the quality that fit the device limit, so the
next frame can start near it instead of re-walking the ladder from the top. */
fn encode_jpeg(image: &DynamicImage, requested_quality: u8) -> Result<(Vec<u8>, u8), String> {
    for quality in (30..=requested_quality.clamp(30, 95)).rev().step_by(5) {
        let mut output = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut Cursor::new(&mut output), quality)
            .encode_image(image)
            .map_err(|error| format!("JPEG encoding failed: {error}"))?;
        if output.len() <= protocol::MAX_JPEG_BYTES {
            return Ok((output, quality));
        }
    }
    Err("JPEG remains larger than the device 100 KiB limit".to_string())
}

/* Crop, scale, and letterbox a raw capture to the device canvas. PipeWire
frames arrive already transformed, so they pass through untouched. */
#[cfg_attr(not(target_os = "linux"), allow(unused_variables))]
fn prepare_image(
    image: DynamicImage,
    session: &CaptureSession,
    config: &StreamConfig,
) -> Result<DynamicImage, String> {
    #[cfg(target_os = "linux")]
    if matches!(session, CaptureSession::PipeWire(_)) {
        return Ok(image);
    }
    crop_and_letterbox(image, config)
}

fn encode_prepared_frame(
    image: &DynamicImage,
    jpeg_quality: u8,
    sequence: u32,
) -> Result<(Vec<u8>, usize, u8), String> {
    let (jpeg, used_quality) = encode_jpeg(image, jpeg_quality)?;
    let jpeg_bytes = jpeg.len();
    Ok((
        protocol::encode_frame(sequence, &jpeg)?,
        jpeg_bytes,
        used_quality,
    ))
}

#[cfg(test)]
fn build_frame(
    image: DynamicImage,
    config: &StreamConfig,
    sequence: u32,
) -> Result<(Vec<u8>, usize), String> {
    let image = crop_and_letterbox(image, config)?;
    let (frame, jpeg_bytes, _) = encode_prepared_frame(&image, config.jpeg_quality, sequence)?;
    Ok((frame, jpeg_bytes))
}

fn open_capture_session(
    app: &AppHandle,
    config: &StreamConfig,
    stop: Arc<AtomicBool>,
) -> Result<CaptureSession, String> {
    if config.source_id == STATIC_COLOR_BARS_ID {
        let (width, height) = config
            .display_orientation
            .logical_size(u32::from(protocol::WIDTH), u32::from(protocol::HEIGHT));
        return Ok(CaptureSession::StaticColorBars(static_color_bars(
            width, height,
        )));
    }
    #[cfg(target_os = "linux")]
    {
        if config.source_id != "linux:portal" {
            return Err("refresh capture sources and choose the Linux portal source".to_string());
        }
        emit(
            app,
            "requesting screen share",
            "Choose a screen or window in the system dialog",
            0,
            0,
            0,
            &config.video_transport,
        );
        let selection = tauri::async_runtime::block_on(wayland_capture::open_portal(
            config.portal_restore_token.as_deref(),
        ))
        .map_err(|error| format!("Screen-share request failed: {error}"))?;
        if let Some(token) = selection.restore_token.as_deref() {
            let _ = app.emit("portal-restore-token", token);
        }
        let source_type = selection
            .source_type
            .map(|kind| format!("{kind:?}"))
            .unwrap_or_else(|| "source".to_string());
        let source_size = selection
            .source_size
            .map(|(width, height)| format!("{width}x{height}"))
            .unwrap_or_else(|| "native size".to_string());
        let is_window =
            selection.source_type == Some(ashpd::desktop::screencast::SourceType::Window);
        eprintln!(
            "Portal selected: type={} logical_size={} node={}",
            source_type, source_size, selection.node_id
        );
        emit(
            app,
            "waiting for PipeWire frames",
            &format!("Waiting for {source_type} at {source_size}"),
            0,
            0,
            0,
            &config.video_transport,
        );
        return Ok(CaptureSession::PipeWire(wayland_capture::start(
            selection.node_id,
            selection.remote,
            is_window,
            stop,
            config.fps.clamp(1, 30),
            wayland_capture::CaptureTransform {
                region_x: config.region_x,
                region_y: config.region_y,
                region_width: config.region_width,
                region_height: config.region_height,
                target_width: u32::from(protocol::WIDTH),
                target_height: u32::from(protocol::HEIGHT),
                orientation: config.display_orientation,
            },
        )));
    }
    #[cfg(not(target_os = "linux"))]
    open_xcap_source(&config.source_id)
}

fn capture_next(session: &CaptureSession) -> Result<DynamicImage, String> {
    match session {
        CaptureSession::StaticColorBars(image) => Ok(image.clone()),
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(receiver) => receiver.receive(Duration::from_secs(10)),
        #[cfg(not(target_os = "linux"))]
        CaptureSession::Monitor(monitor) => Ok(DynamicImage::ImageRgba8(
            monitor.capture_image().map_err(xcap_error)?,
        )),
        #[cfg(not(target_os = "linux"))]
        CaptureSession::Window(window) => {
            let mut window = window
                .lock()
                .map_err(|_| "capture window lock failed".to_string())?;
            window.refresh().map_err(xcap_error)?;
            Ok(DynamicImage::ImageRgba8(
                window.capture_image().map_err(xcap_error)?,
            ))
        }
    }
}

fn capture_next_optional(
    session: &CaptureSession,
    timeout: Duration,
) -> Result<Option<DynamicImage>, String> {
    match session {
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(receiver) => receiver.receive_optional(timeout),
        _ => capture_next(session).map(Some),
    }
}

fn image_fingerprint(image: &DynamicImage) -> u32 {
    crc32fast::hash(image.to_rgb8().as_raw())
}

fn base64_encode(data: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut output = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let bits = (u32::from(chunk[0]) << 16)
            | (u32::from(*chunk.get(1).unwrap_or(&0)) << 8)
            | u32::from(*chunk.get(2).unwrap_or(&0));
        output.push(ALPHABET[(bits >> 18) as usize & 63] as char);
        output.push(ALPHABET[(bits >> 12) as usize & 63] as char);
        output.push(if chunk.len() > 1 {
            ALPHABET[(bits >> 6) as usize & 63] as char
        } else {
            '='
        });
        output.push(if chunk.len() > 2 {
            ALPHABET[bits as usize & 63] as char
        } else {
            '='
        });
    }
    output
}

const PREVIEW_INTERVAL: Duration = Duration::from_secs(1);

/* Mirrors the most recently sent JPEG into the UI so the user can confirm
what the device shows without looking at it. */
fn emit_preview(app: &AppHandle, frame: &[u8], last_preview: &mut Instant) {
    if last_preview.elapsed() < PREVIEW_INTERVAL || frame.len() <= protocol::HEADER_LEN {
        return;
    }
    *last_preview = Instant::now();
    let _ = app.emit(
        "stream-preview",
        base64_encode(&frame[protocol::HEADER_LEN..]),
    );
}

fn replay_frame(frame: &[u8], sequence: u32) -> Result<Vec<u8>, String> {
    if frame.len() < protocol::HEADER_LEN || &frame[..4] != b"CDP1" {
        return Err("cached frame is invalid".to_string());
    }
    let mut replay = frame.to_vec();
    replay[8..12].copy_from_slice(&sequence.to_le_bytes());
    Ok(replay)
}

fn emit(
    app: &AppHandle,
    state: &str,
    detail: impl Into<String>,
    fps: u8,
    jpeg_bytes: usize,
    dropped_frames: u32,
    video_transport: &str,
) {
    emit_metrics(
        app,
        state,
        detail,
        fps,
        jpeg_bytes,
        dropped_frames,
        video_transport,
        0,
        0,
        0,
        0,
    );
}

fn emit_metrics(
    app: &AppHandle,
    state: &str,
    detail: impl Into<String>,
    fps: u8,
    jpeg_bytes: usize,
    dropped_frames: u32,
    video_transport: &str,
    capture_encode_ms: u32,
    capture_wait_ms: u32,
    device_ms: u32,
    device_replaced_frames: u32,
) {
    let event = StreamEvent {
        state: state.into(),
        detail: detail.into(),
        fps,
        jpeg_bytes,
        dropped_frames,
        video_transport: video_transport.into(),
        capture_encode_ms,
        capture_wait_ms,
        device_ms,
        device_replaced_frames,
    };
    if let Ok(mut current) = app.state::<StreamState>().event.lock() {
        *current = event.clone();
    }
    let _ = app.emit("stream-status", event);
}

fn wait_for_presented(
    socket: &mut WebSocket<TcpStream>,
    sequence: u32,
    stop: &AtomicBool,
) -> Result<FramePresented, String> {
    socket
        .get_mut()
        .set_read_timeout(Some(TCP_PRESENTED_POLL_INTERVAL))
        .map_err(|error| format!("Cannot configure device feedback timeout: {error}"))?;
    loop {
        if stop.load(Ordering::Relaxed) {
            return Err("Stream stopped".to_string());
        }
        let message = match socket.read() {
            Ok(message) => message,
            Err(WebSocketError::Io(error))
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) =>
            {
                continue;
            }
            Err(error) => return Err(format!("Cannot receive device frame feedback: {error}")),
        };
        match message {
            Message::Text(text) => {
                if let Ok(feedback) = serde_json::from_str::<FramePresented>(&text) {
                    if feedback.kind == "frame_presented" && feedback.sequence == sequence {
                        return Ok(feedback);
                    }
                }
            }
            Message::Close(_) => return Err("Device closed the control connection".to_string()),
            _ => {}
        }
    }
}

fn close_device_stream(socket: &mut WebSocket<TcpStream>) {
    let _ = socket.get_mut().set_nonblocking(false);
    let _ = socket.send(Message::Text(
        json!({"type": "stop_stream", "request_id": "stop-1"})
            .to_string()
            .into(),
    ));
    let _ = socket.close(None);
}

fn active_stream_error(active: Option<&Arc<AtomicBool>>) -> Option<&'static str> {
    match active {
        Some(stop) if stop.load(Ordering::Relaxed) => {
            Some("previous Wi-Fi stream is still stopping")
        }
        Some(_) => Some("a Wi-Fi stream is already running"),
        None => None,
    }
}

fn describe_websocket_error(error: WebSocketError) -> String {
    match error {
        WebSocketError::Http(response) if response.status().as_u16() == 401 => {
            "Pairing code rejected. Check the 6-digit code shown on the device.".to_string()
        }
        WebSocketError::Http(response) if response.status().as_u16() == 403 => {
            "Device is busy with another transport.".to_string()
        }
        error => format!("WebSocket handshake failed: {error}"),
    }
}

fn connect_wifi(endpoint: &str) -> Result<WebSocket<TcpStream>, String> {
    let uri = endpoint
        .parse::<tungstenite::http::Uri>()
        .map_err(|error| format!("Invalid device address: {error}"))?;
    let host = uri
        .host()
        .ok_or_else(|| "Device address has no host".to_string())?;
    let port = uri.port_u16().unwrap_or(80);
    let addresses = (host, port)
        .to_socket_addrs()
        .map_err(|error| format!("Cannot resolve device address: {error}"))?;
    let mut last_error = None;
    for address in addresses {
        match TcpStream::connect_timeout(&address, CONNECTION_TIMEOUT) {
            Ok(stream) => {
                stream
                    .set_nodelay(true)
                    .map_err(|error| format!("Cannot configure connection: {error}"))?;
                stream
                    .set_read_timeout(Some(CONNECTION_TIMEOUT))
                    .map_err(|error| format!("Cannot configure connection: {error}"))?;
                stream
                    .set_write_timeout(Some(CONNECTION_TIMEOUT))
                    .map_err(|error| format!("Cannot configure connection: {error}"))?;
                return match client(endpoint, stream) {
                    Ok((socket, _)) => Ok(socket),
                    Err(HandshakeError::Failure(error)) => Err(describe_websocket_error(error)),
                    Err(HandshakeError::Interrupted(_)) => {
                        Err("WebSocket handshake was interrupted".to_string())
                    }
                };
            }
            Err(error) => last_error = Some(error),
        }
    }
    Err(format!(
        "Cannot reach device within {} seconds{}",
        CONNECTION_TIMEOUT.as_secs(),
        last_error
            .map(|error| format!(": {error}"))
            .unwrap_or_default(),
    ))
}

fn stream_endpoint(config: &StreamConfig) -> String {
    format!(
        "{}{}token={}",
        config.endpoint.trim_end_matches('/'),
        if config.endpoint.contains('?') {
            "&"
        } else {
            "?"
        },
        config.token
    )
}

fn configure_udp_control_socket(socket: &mut WebSocket<TcpStream>) -> Result<(), String> {
    socket
        .get_mut()
        .set_nonblocking(true)
        .map_err(|error| format!("Cannot configure UDP control socket: {error}"))
}

fn service_udp_control(
    socket: &mut WebSocket<TcpStream>,
    heartbeat: &mut UdpControlHeartbeat,
) -> Result<(), String> {
    loop {
        match socket.read() {
            Ok(Message::Text(text)) => {
                let is_pending_pong =
                    serde_json::from_str::<Value>(&text)
                        .ok()
                        .is_some_and(|message| {
                            message.get("type").and_then(Value::as_str) == Some("pong")
                                && heartbeat.pending_request_id.as_deref()
                                    == message.get("request_id").and_then(Value::as_str)
                        });
                if is_pending_pong {
                    heartbeat.pending_request_id = None;
                    heartbeat.misses = 0;
                }
            }
            Ok(Message::Ping(payload)) => socket
                .send(Message::Pong(payload))
                .map_err(|error| format!("Cannot respond to device heartbeat: {error}"))?,
            Ok(Message::Close(_)) => return Err("Device closed the control connection".to_string()),
            Ok(_) => {}
            Err(WebSocketError::Io(error)) if error.kind() == std::io::ErrorKind::WouldBlock => {
                break
            }
            Err(WebSocketError::ConnectionClosed) | Err(WebSocketError::AlreadyClosed) => {
                return Err("Device closed the control connection".to_string());
            }
            Err(error) => return Err(format!("UDP control connection failed: {error}")),
        }
    }

    let now = Instant::now();
    if now < heartbeat.next_ping {
        return Ok(());
    }
    if heartbeat.pending_request_id.take().is_some() {
        heartbeat.misses += 1;
        if heartbeat.misses >= CONTROL_HEARTBEAT_MISSES {
            return Err("Control heartbeat timed out".to_string());
        }
    }
    heartbeat.sequence = heartbeat.sequence.wrapping_add(1);
    let request_id = format!("ping-{}", heartbeat.sequence);
    socket
        .send(Message::Text(
            json!({"type": "ping", "request_id": request_id})
                .to_string()
                .into(),
        ))
        .map_err(|error| format!("Cannot send control heartbeat: {error}"))?;
    heartbeat.pending_request_id = Some(format!("ping-{}", heartbeat.sequence));
    heartbeat.next_ping = now + CONTROL_HEARTBEAT_INTERVAL;
    Ok(())
}

fn reconnect_udp_control(
    app: &AppHandle,
    config: &StreamConfig,
    stop: &AtomicBool,
    reason: &str,
    dropped_frames: u32,
) -> Result<(WebSocket<TcpStream>, VideoSender), String> {
    let endpoint = stream_endpoint(config);
    let mut last_error = reason.to_string();
    for (index, delay) in CONTROL_RECONNECT_DELAYS.iter().enumerate() {
        emit(
            app,
            "reconnecting",
            format!(
                "Reconnecting control channel ({}/{})",
                index + 1,
                CONTROL_RECONNECT_DELAYS.len()
            ),
            0,
            0,
            dropped_frames,
            "udp",
        );
        let deadline = Instant::now() + *delay;
        while !stop.load(Ordering::Relaxed) && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(50));
        }
        if stop.load(Ordering::Relaxed) {
            return Err("Stream stopped".to_string());
        }
        match connect_wifi(&endpoint).and_then(|mut socket| {
            let sender = negotiate_stream(&mut socket, "udp")?;
            configure_udp_control_socket(&mut socket)?;
            Ok((socket, sender))
        }) {
            Ok(connection) => {
                emit(
                    app,
                    "streaming",
                    "UDP control channel reconnected",
                    0,
                    0,
                    dropped_frames,
                    "udp",
                );
                return Ok(connection);
            }
            Err(error) => last_error = error,
        }
    }
    Err(format!(
        "Control reconnection failed after {} attempts: {last_error}",
        CONTROL_RECONNECT_DELAYS.len()
    ))
}

fn control_request(
    socket: &mut WebSocket<TcpStream>,
    request: Value,
    request_id: &str,
) -> Result<ControlReply, String> {
    socket
        .send(Message::Text(request.to_string().into()))
        .map_err(|error| format!("Cannot send control request: {error}"))?;
    loop {
        let message = socket
            .read()
            .map_err(|error| format!("Cannot receive control response: {error}"))?;
        match message {
            Message::Text(text) => {
                let reply: ControlReply = serde_json::from_str(&text)
                    .map_err(|error| format!("Device returned invalid control data: {error}"))?;
                if reply.request_id != request_id {
                    continue;
                }
                if reply.kind == "error" {
                    return Err(format!("Device rejected {request_id}: {}", reply.code));
                }
                return Ok(reply);
            }
            Message::Close(_) => return Err("Device closed the control connection".to_string()),
            _ => continue,
        }
    }
}

fn negotiate_stream(
    socket: &mut WebSocket<TcpStream>,
    requested_transport: &str,
) -> Result<VideoSender, String> {
    let hello = control_request(
        socket,
        json!({
            "type": "hello", "request_id": "hello-1", "protocol_version": CONTROL_PROTOCOL_VERSION,
            "video_transport": requested_transport, "capabilities": {"touch": true, "audio_capture": true, "text_input": true}
        }),
        "hello-1",
    )?;
    if hello.kind != "hello_ack" || hello.video_transport != requested_transport {
        return Err("Device did not confirm the requested video transport".to_string());
    }
    let sender = if requested_transport == "udp" {
        let parameters = hello
            .udp
            .ok_or_else(|| "Device did not provide UDP session parameters".to_string())?;
        if parameters.max_payload == 0 || parameters.max_payload > 1200 {
            return Err("Device provided an invalid UDP payload limit".to_string());
        }
        let control = socket.get_ref();
        let local_ip = control
            .local_addr()
            .map_err(|error| format!("Cannot read local control address: {error}"))?
            .ip();
        let remote_ip = control
            .peer_addr()
            .map_err(|error| format!("Cannot read device control address: {error}"))?
            .ip();
        let (IpAddr::V4(local_ip), IpAddr::V4(remote_ip)) = (local_ip, remote_ip) else {
            return Err("UDP video requires an IPv4 control connection".to_string());
        };
        let udp = UdpSocket::bind(SocketAddr::new(IpAddr::V4(local_ip), 0))
            .map_err(|error| format!("Cannot create UDP socket: {error}"))?;
        udp.connect(SocketAddr::new(IpAddr::V4(remote_ip), parameters.port))
            .map_err(|error| format!("Cannot connect UDP socket: {error}"))?;
        let udp_local = udp
            .local_addr()
            .map_err(|error| format!("Cannot read UDP local address: {error}"))?;
        let udp_remote = udp
            .peer_addr()
            .map_err(|error| format!("Cannot read UDP remote address: {error}"))?;
        eprintln!(
            "UDP video route: TCP {} -> {}; UDP {} -> {}",
            control.local_addr().unwrap_or(udp_local),
            control.peer_addr().unwrap_or(udp_remote),
            udp_local,
            udp_remote
        );
        VideoSender::Udp {
            socket: udp,
            parameters,
            sent_packets: 0,
            sent_bytes: 0,
        }
    } else {
        VideoSender::Tcp
    };
    let started = control_request(
        socket,
        json!({"type": "start_stream", "request_id": "start-1"}),
        "start-1",
    )?;
    if started.kind != "stream_started" || started.video_transport != requested_transport {
        return Err("Device did not start the requested video transport".to_string());
    }
    Ok(sender)
}

fn parse_http_response(response: &[u8]) -> Result<Option<(usize, usize)>, String> {
    let Some(header_end) = response.windows(4).position(|window| window == b"\r\n\r\n") else {
        return Ok(None);
    };
    let headers = std::str::from_utf8(&response[..header_end])
        .map_err(|_| "Device returned non-text HTTP headers".to_string())?;
    let status = headers.lines().next().unwrap_or_default();
    if status.split_whitespace().nth(1) != Some("200") {
        return Err(format!("Device status request failed: {status}"));
    }
    let content_length = headers
        .lines()
        .skip(1)
        .find_map(|line| {
            line.split_once(':')
                .filter(|(name, _)| name.trim().eq_ignore_ascii_case("content-length"))
        })
        .map(|(_, value)| {
            value
                .trim()
                .parse::<usize>()
                .map_err(|_| "Device returned an invalid Content-Length".to_string())
        })
        .transpose()?
        .ok_or_else(|| "Device status response has no Content-Length".to_string())?;
    Ok(Some((header_end + 4, content_length)))
}

fn read_http_response(stream: &mut TcpStream) -> Result<Vec<u8>, String> {
    const MAX_RESPONSE_BYTES: usize = 4096;
    let mut response = Vec::new();
    loop {
        if let Some((body_start, content_length)) = parse_http_response(&response)? {
            let response_end = body_start
                .checked_add(content_length)
                .ok_or_else(|| "Device status response is too large".to_string())?;
            if response_end > MAX_RESPONSE_BYTES {
                return Err("Device status response is too large".to_string());
            }
            if response.len() >= response_end {
                response.truncate(response_end);
                return Ok(response);
            }
        }
        if response.len() == MAX_RESPONSE_BYTES {
            return Err("Device status response is too large".to_string());
        }
        let mut chunk = [0_u8; 512];
        let max_read = chunk.len().min(MAX_RESPONSE_BYTES - response.len());
        let read = stream
            .read(&mut chunk[..max_read])
            .map_err(|error| format!("Cannot read device status: {error}"))?;
        if read == 0 {
            return Err("Device closed the status response early".to_string());
        }
        response.extend_from_slice(&chunk[..read]);
    }
}

fn probe_wifi_device_impl(endpoint: &str) -> Result<DeviceStatus, String> {
    let uri = endpoint
        .parse::<tungstenite::http::Uri>()
        .map_err(|error| format!("Invalid device address: {error}"))?;
    if uri.scheme_str() != Some("ws") {
        return Err("Device address must start with ws://".to_string());
    }
    let host = uri
        .host()
        .ok_or_else(|| "Device address has no host".to_string())?;
    let port = uri.port_u16().unwrap_or(80);
    let address = (host, port)
        .to_socket_addrs()
        .map_err(|error| format!("Cannot resolve device address: {error}"))?
        .next()
        .ok_or_else(|| "Device address has no reachable network address".to_string())?;
    let mut stream = TcpStream::connect_timeout(&address, CONNECTION_TIMEOUT).map_err(|error| {
        format!(
            "Cannot reach device within {} seconds: {error}",
            CONNECTION_TIMEOUT.as_secs()
        )
    })?;
    stream
        .set_read_timeout(Some(CONNECTION_TIMEOUT))
        .map_err(|error| format!("Cannot configure connection: {error}"))?;
    stream
        .set_write_timeout(Some(CONNECTION_TIMEOUT))
        .map_err(|error| format!("Cannot configure connection: {error}"))?;
    let host_header = if port == 80 {
        host.to_string()
    } else {
        format!("{host}:{port}")
    };
    let request =
        format!("GET /api/status HTTP/1.1\r\nHost: {host_header}\r\nConnection: close\r\n\r\n");
    stream
        .write_all(request.as_bytes())
        .map_err(|error| format!("Cannot request device status: {error}"))?;
    let response = read_http_response(&mut stream)?;
    let (body_start, _) = parse_http_response(&response)?
        .ok_or_else(|| "Device returned an incomplete HTTP response".to_string())?;
    serde_json::from_slice(&response[body_start..])
        .map_err(|error| format!("Device returned invalid status data: {error}"))
}

fn send_video_frame(
    socket: &mut WebSocket<TcpStream>,
    sender: &mut VideoSender,
    frame: Vec<u8>,
    sequence: u32,
) -> Result<(), String> {
    match sender {
        VideoSender::Tcp => socket
            .send(Message::Binary(frame.into()))
            .map_err(|error| format!("WebSocket send failed: {error}")),
        VideoSender::Udp {
            socket: udp,
            parameters,
            sent_packets,
            sent_bytes,
        } => {
            let started = Instant::now();
            let fragments = protocol::encode_udp_fragments(
                &frame,
                parameters.session_id,
                parameters.session_token,
                sequence,
                parameters.max_payload,
            )?;
            let fragment_count = fragments.len();
            for (index, datagram) in fragments.into_iter().enumerate() {
                let sent = udp
                    .send(&datagram)
                    .map_err(|error| format!("UDP send failed: {error}"))?;
                if sent != datagram.len() {
                    return Err(format!(
                        "UDP send was truncated: {sent} of {} bytes",
                        datagram.len()
                    ));
                }
                *sent_packets += 1;
                *sent_bytes += sent as u64;
                if index > 0 && started.elapsed() > UDP_FRAME_SEND_LIMIT {
                    return Err("UDP frame exceeded the 120 ms send budget".to_string());
                }
                if index + 1 < fragment_count && (index + 1) % UDP_FRAGMENTS_PER_TICK == 0 {
                    thread::sleep(UDP_FRAGMENT_SPACING);
                }
            }
            Ok(())
        }
    }
}

fn udp_sender_stats(sender: &VideoSender) -> (u64, u64) {
    match sender {
        VideoSender::Udp {
            sent_packets,
            sent_bytes,
            ..
        } => (*sent_packets, *sent_bytes),
        VideoSender::Tcp => (0, 0),
    }
}

fn run_stream(
    app: AppHandle,
    config: StreamConfig,
    stop: Arc<AtomicBool>,
    live: Arc<LiveParams>,
    mut socket: WebSocket<TcpStream>,
    mut sender: VideoSender,
) -> u32 {
    let video_transport = config.video_transport.as_str();
    let mut fps = config.fps.clamp(1, 30);
    let mut jpeg_quality = config.jpeg_quality.clamp(30, 95);
    live.fps.store(fps, Ordering::Relaxed);
    live.quality.store(jpeg_quality, Ordering::Relaxed);
    let mut frame_interval = Duration::from_millis(1000 / u64::from(fps));
    /* Start each encode near the last quality that fit the 100 KiB limit
    instead of re-walking the ladder from the user setting every frame. */
    let mut adaptive_quality = jpeg_quality;
    let mut dropped_frames = 0_u32;
    let mut heartbeat = if matches!(sender, VideoSender::Udp { .. }) {
        if let Err(error) = configure_udp_control_socket(&mut socket) {
            emit(&app, "error", error, 0, 0, 0, video_transport);
            close_device_stream(&mut socket);
            return dropped_frames;
        }
        Some(UdpControlHeartbeat::new())
    } else {
        None
    };
    let mut sequence = 0_u32;
    let capture = match open_capture_session(&app, &config, stop.clone()) {
        Ok(capture) => capture,
        Err(error) => {
            emit(&app, "error", error, 0, 0, 0, video_transport);
            close_device_stream(&mut socket);
            return dropped_frames;
        }
    };
    emit(
        &app,
        "capturing",
        "Capturing first frame",
        0,
        0,
        0,
        video_transport,
    );
    let capture_wait_started = Instant::now();
    let image = match capture_next(&capture) {
        Ok(image) => image,
        Err(error) => {
            if !stop.load(Ordering::Relaxed) {
                emit(
                    &app,
                    "error",
                    format!("First screenshot failed: {error}"),
                    0,
                    0,
                    0,
                    video_transport,
                );
            }
            close_device_stream(&mut socket);
            return dropped_frames;
        }
    };
    if stop.load(Ordering::Relaxed) {
        close_device_stream(&mut socket);
        return dropped_frames;
    }
    let first_capture_wait_ms = capture_wait_started.elapsed().as_millis() as u32;
    let encode_started = Instant::now();
    let (frame, jpeg_bytes, first_fingerprint): (Vec<u8>, usize, u32) =
        match prepare_image(image, &capture, &config).and_then(|prepared| {
            let fingerprint = image_fingerprint(&prepared);
            encode_prepared_frame(&prepared, adaptive_quality, sequence).map(
                |(frame, jpeg_bytes, used_quality)| (frame, jpeg_bytes, used_quality, fingerprint),
            )
        }) {
            Ok((frame, jpeg_bytes, used_quality, fingerprint)) => {
                adaptive_quality = used_quality.saturating_add(5).min(jpeg_quality);
                (frame, jpeg_bytes, fingerprint)
            }
            Err(error) => {
                if !stop.load(Ordering::Relaxed) {
                    emit(
                        &app,
                        "error",
                        format!("First screenshot failed: {error}"),
                        0,
                        0,
                        0,
                        video_transport,
                    );
                }
                close_device_stream(&mut socket);
                return dropped_frames;
            }
        };
    let first_capture_encode_ms = encode_started.elapsed().as_millis() as u32;
    let mut cached_frame = frame.clone();
    let mut last_fingerprint = first_fingerprint;
    let mut last_preview = Instant::now() - PREVIEW_INTERVAL;
    emit_preview(&app, &frame, &mut last_preview);
    emit(
        &app,
        "frame ready",
        "First shared frame captured",
        0,
        jpeg_bytes,
        0,
        video_transport,
    );
    if let Err(error) = send_video_frame(&mut socket, &mut sender, frame, sequence) {
        if !stop.load(Ordering::Relaxed) {
            emit(
                &app,
                "error",
                format!("First screenshot send failed: {error}"),
                0,
                jpeg_bytes,
                0,
                video_transport,
            );
        }
        close_device_stream(&mut socket);
        return dropped_frames;
    }
    let mut last_video_sent = Instant::now();
    let mut static_frames = 0_u32;
    let mut device_ms = 0;
    let mut device_replaced_frames = 0;
    if matches!(sender, VideoSender::Tcp) {
        match wait_for_presented(&mut socket, sequence, &stop) {
            Ok(feedback) => {
                device_ms = (feedback.queue_wait_us
                    + feedback.jpeg_decode_us
                    + feedback.lcd_wait_us
                    + feedback.lcd_submit_us)
                    / 1000;
                device_replaced_frames = feedback.replaced_frames;
                eprintln!("Frame {} presented: queue={}us decode={}us lcd_wait={}us dma={}us previous_dma={}us", feedback.sequence, feedback.queue_wait_us, feedback.jpeg_decode_us, feedback.lcd_wait_us, feedback.lcd_submit_us, feedback.previous_dma_us);
            }
            Err(error) => {
                if !stop.load(Ordering::Relaxed) {
                    emit(&app, "error", error, 0, jpeg_bytes, 0, video_transport);
                }
                close_device_stream(&mut socket);
                return dropped_frames;
            }
        }
    }
    sequence = sequence.wrapping_add(1);
    let (udp_packets, udp_bytes) = udp_sender_stats(&sender);
    emit_metrics(
        &app,
        "snapshot sent",
        if video_transport == "udp" {
            format!("First screenshot sent over UDP ({udp_packets} datagrams, {udp_bytes} bytes)")
        } else {
            "First screenshot sent over TCP".to_string()
        },
        0,
        jpeg_bytes,
        0,
        video_transport,
        first_capture_encode_ms,
        first_capture_wait_ms,
        device_ms,
        device_replaced_frames,
    );
    let mut next_frame = Instant::now() + frame_interval;
    while !stop.load(Ordering::Relaxed) {
        let now = Instant::now();
        if now < next_frame {
            thread::sleep(next_frame - now);
        }
        next_frame = Instant::now() + frame_interval;
        let live_fps = live.fps.load(Ordering::Relaxed).clamp(1, 30);
        if live_fps != fps {
            fps = live_fps;
            frame_interval = Duration::from_millis(1000 / u64::from(fps));
        }
        let live_quality = live.quality.load(Ordering::Relaxed).clamp(30, 95);
        if live_quality != jpeg_quality {
            jpeg_quality = live_quality;
            adaptive_quality = jpeg_quality;
            /* Force a re-encode so the new quality applies to a static scene too. */
            last_fingerprint = last_fingerprint.wrapping_add(1);
        }
        if let Some(heartbeat) = heartbeat.as_mut() {
            if let Err(reason) = service_udp_control(&mut socket, heartbeat) {
                match reconnect_udp_control(&app, &config, &stop, &reason, dropped_frames) {
                    Ok((new_socket, new_sender)) => {
                        socket = new_socket;
                        sender = new_sender;
                        *heartbeat = UdpControlHeartbeat::new();
                        last_video_sent = Instant::now() - STATIC_FRAME_INTERVAL;
                        continue;
                    }
                    Err(error) => {
                        emit(&app, "error", error, 0, 0, dropped_frames, video_transport);
                        break;
                    }
                }
            }
        }
        let capture_wait_started = Instant::now();
        let image = match capture_next_optional(&capture, frame_interval) {
            Ok(image) => image,
            Err(error) => {
                dropped_frames += 1;
                emit(
                    &app,
                    "dropping",
                    error,
                    0,
                    0,
                    dropped_frames,
                    video_transport,
                );
                continue;
            }
        };
        let capture_wait_ms = capture_wait_started.elapsed().as_millis() as u32;
        let encode_started = Instant::now();
        /* Fingerprint the already cropped/scaled 368x448 image exactly once
        per captured frame; a raw desktop-sized hash costs far more. */
        let prepared = match image {
            Some(image) => match prepare_image(image, &capture, &config) {
                Ok(prepared) => {
                    let fingerprint = image_fingerprint(&prepared);
                    (fingerprint != last_fingerprint).then_some((prepared, fingerprint))
                }
                Err(error) => {
                    dropped_frames += 1;
                    emit(
                        &app,
                        "dropping",
                        error,
                        0,
                        0,
                        dropped_frames,
                        video_transport,
                    );
                    continue;
                }
            },
            None => None,
        };
        let static_frame = prepared.is_none();
        let (frame, jpeg_bytes): (Vec<u8>, usize) = match prepared {
            Some((prepared, fingerprint)) => {
                match encode_prepared_frame(&prepared, adaptive_quality, sequence) {
                    Ok((frame, jpeg_bytes, used_quality)) => {
                        adaptive_quality = used_quality.saturating_add(5).min(jpeg_quality);
                        cached_frame = frame.clone();
                        last_fingerprint = fingerprint;
                        static_frames = 0;
                        (frame, jpeg_bytes)
                    }
                    Err(error) => {
                        dropped_frames += 1;
                        emit(
                            &app,
                            "dropping",
                            error,
                            0,
                            0,
                            dropped_frames,
                            video_transport,
                        );
                        continue;
                    }
                }
            }
            None => {
                if last_video_sent.elapsed() < STATIC_FRAME_INTERVAL {
                    continue;
                }
                match replay_frame(&cached_frame, sequence) {
                    Ok(frame) => (frame, cached_frame.len() - protocol::HEADER_LEN),
                    Err(error) => {
                        emit(&app, "error", error, 0, 0, dropped_frames, video_transport);
                        break;
                    }
                }
            }
        };
        emit_preview(&app, &frame, &mut last_preview);
        if let Err(error) = send_video_frame(&mut socket, &mut sender, frame, sequence) {
            if matches!(sender, VideoSender::Udp { .. }) {
                match reconnect_udp_control(&app, &config, &stop, &error, dropped_frames) {
                    Ok((new_socket, new_sender)) => {
                        socket = new_socket;
                        sender = new_sender;
                        heartbeat = Some(UdpControlHeartbeat::new());
                        last_video_sent = Instant::now() - STATIC_FRAME_INTERVAL;
                        continue;
                    }
                    Err(reconnect_error) => {
                        emit(
                            &app,
                            "error",
                            reconnect_error,
                            0,
                            jpeg_bytes,
                            dropped_frames,
                            video_transport,
                        );
                        break;
                    }
                }
            }
            emit(
                &app,
                "error",
                error,
                0,
                jpeg_bytes,
                dropped_frames,
                video_transport,
            );
            break;
        }
        let capture_encode_ms = encode_started.elapsed().as_millis() as u32;
        if matches!(sender, VideoSender::Tcp) {
            match wait_for_presented(&mut socket, sequence, &stop) {
                Ok(feedback) => {
                    device_ms = (feedback.queue_wait_us
                        + feedback.jpeg_decode_us
                        + feedback.lcd_wait_us
                        + feedback.lcd_submit_us)
                        / 1000;
                    device_replaced_frames = feedback.replaced_frames;
                }
                Err(error) => {
                    if !stop.load(Ordering::Relaxed) {
                        emit_metrics(
                            &app,
                            "error",
                            error,
                            0,
                            jpeg_bytes,
                            dropped_frames,
                            video_transport,
                            capture_encode_ms,
                            capture_wait_ms,
                            device_ms,
                            device_replaced_frames,
                        );
                    }
                    break;
                }
            }
        }
        last_video_sent = Instant::now();
        if static_frame {
            static_frames += 1;
        }
        sequence = sequence.wrapping_add(1);
        let (udp_packets, udp_bytes) = udp_sender_stats(&sender);
        emit_metrics(
            &app,
            "streaming",
            if static_frame {
                format!("Static 1 FPS ({static_frames} cached frames)")
            } else if video_transport == "udp" {
                format!("Sending UDP video ({udp_packets} datagrams, {udp_bytes} bytes)")
            } else {
                "Sending TCP video frames".to_string()
            },
            fps,
            jpeg_bytes,
            dropped_frames,
            video_transport,
            capture_encode_ms,
            capture_wait_ms,
            device_ms,
            device_replaced_frames,
        );
    }
    close_device_stream(&mut socket);
    dropped_frames
}

#[tauri::command]
fn list_capture_sources() -> Result<Vec<CaptureSourceInfo>, String> {
    list_capture_sources_impl()
}

#[tauri::command]
fn get_stream_status(state: State<'_, StreamState>) -> StreamEvent {
    state
        .event
        .lock()
        .map(|event| event.clone())
        .unwrap_or_default()
}

#[tauri::command]
fn probe_wifi_device(endpoint: String) -> Result<DeviceStatus, String> {
    probe_wifi_device_impl(endpoint.trim())
}

#[tauri::command]
fn start_wifi_stream(
    app: AppHandle,
    mut config: StreamConfig,
    state: State<'_, StreamState>,
) -> Result<(), String> {
    config.endpoint = config.endpoint.trim().to_owned();
    config.token = config.token.trim().to_owned();
    config.video_transport = config.video_transport.trim().to_ascii_lowercase();
    if !config.endpoint.starts_with("ws://")
        || config.token.len() != 6
        || !config.token.bytes().all(|byte| byte.is_ascii_digit())
    {
        return Err("enter a ws:// device address and its 6-digit pairing code".to_string());
    }
    if !matches!(config.video_transport.as_str(), "tcp" | "udp") {
        return Err("select TCP or UDP video transport".to_string());
    }
    {
        let guard = state.stop.lock().map_err(|_| "stream state unavailable")?;
        if let Some(error) = active_stream_error(guard.as_ref()) {
            return Err(error.to_string());
        }
    }
    let device = probe_wifi_device_impl(&config.endpoint)?;
    if device.protocol_version != CONTROL_PROTOCOL_VERSION {
        return Err(format!(
            "Device protocol v{} is incompatible. Flash firmware with control protocol v{} before streaming.",
            device.protocol_version, CONTROL_PROTOCOL_VERSION
        ));
    }
    if !device
        .video_transports
        .iter()
        .any(|transport| transport == &config.video_transport)
    {
        return Err(format!(
            "Device does not support {} video",
            config.video_transport.to_uppercase()
        ));
    }
    if device.wifi != "connected" {
        return Err("Device Wi-Fi is not connected".to_string());
    }
    if device.active_transport != "none" {
        return Err(format!(
            "Device is busy with {} transport",
            device.active_transport
        ));
    }
    let valid_source = config.source_id == STATIC_COLOR_BARS_ID
        || config.source_id == "linux:portal"
        || config
            .source_id
            .split_once(':')
            .is_some_and(|(kind, index)| {
                matches!(kind, "monitor" | "window") && index.parse::<usize>().is_ok()
            });
    if !valid_source {
        return Err("select a capture source before starting the stream".to_string());
    }
    emit(
        &app,
        "connecting",
        "Opening Wi-Fi control connection",
        0,
        0,
        0,
        &config.video_transport,
    );
    let mut socket = connect_wifi(&stream_endpoint(&config))?;
    emit(
        &app,
        "negotiating",
        format!(
            "Requesting {} video transport",
            config.video_transport.to_uppercase()
        ),
        0,
        0,
        0,
        &config.video_transport,
    );
    let sender = negotiate_stream(&mut socket, &config.video_transport)?;
    let mut guard = state.stop.lock().map_err(|_| "stream state unavailable")?;
    if let Some(error) = active_stream_error(guard.as_ref()) {
        close_device_stream(&mut socket);
        return Err(error.to_string());
    }
    let stop = Arc::new(AtomicBool::new(false));
    *guard = Some(stop.clone());
    drop(guard);
    let controller = state.stop.clone();
    let live = state.live.clone();
    let final_app = app.clone();
    let final_transport = config.video_transport.clone();
    let final_stop = stop.clone();
    thread::spawn(move || {
        let dropped_frames = run_stream(app, config, stop, live, socket, sender);
        if let Ok(mut active) = controller.lock() {
            *active = None;
        }
        if final_stop.load(Ordering::Relaxed) {
            emit(
                &final_app,
                "stopped",
                "Wi-Fi stream stopped",
                0,
                0,
                dropped_frames,
                &final_transport,
            );
        }
    });
    Ok(())
}

#[tauri::command]
fn set_stream_params(
    fps: u8,
    jpeg_quality: u8,
    state: State<'_, StreamState>,
) -> Result<(), String> {
    if !(1..=30).contains(&fps) {
        return Err("FPS must be between 1 and 30".to_string());
    }
    if !(30..=95).contains(&jpeg_quality) {
        return Err("JPEG quality must be between 30 and 95".to_string());
    }
    state.live.fps.store(fps, Ordering::Relaxed);
    state.live.quality.store(jpeg_quality, Ordering::Relaxed);
    Ok(())
}

#[tauri::command]
fn stop_wifi_stream(app: AppHandle, state: State<'_, StreamState>) -> Result<(), String> {
    let active = state.stop.lock().map_err(|_| "stream state unavailable")?;
    let transport = state
        .event
        .lock()
        .ok()
        .map(|event| event.video_transport.clone())
        .filter(|transport| transport != "none")
        .unwrap_or_else(|| "none".to_string());
    match active.as_ref() {
        Some(stop) => {
            let was_stopping = stop.swap(true, Ordering::Relaxed);
            emit(
                &app,
                "stopping",
                if was_stopping {
                    "Wi-Fi stream is already stopping"
                } else {
                    "Stopping Wi-Fi stream and releasing device transport"
                },
                0,
                0,
                0,
                &transport,
            );
        }
        None => emit(&app, "stopped", "Stream stopped", 0, 0, 0, "none"),
    }
    Ok(())
}

fn main() {
    #[cfg(target_os = "linux")]
    std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(StreamState::default())
        .invoke_handler(tauri::generate_handler![
            list_capture_sources,
            probe_wifi_device,
            get_stream_status,
            start_wifi_stream,
            set_stream_params,
            stop_wifi_stream
        ])
        .run(tauri::generate_context!())
        .expect("failed to run Companion Display");
}

#[cfg(test)]
mod tests {
    use image::GenericImageView;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;

    use super::{
        active_stream_error, base64_encode, build_frame, encode_jpeg, parse_http_response,
        replay_frame, static_color_bars, DisplayOrientation, StreamConfig,
    };
    use crate::protocol;

    fn test_config() -> StreamConfig {
        StreamConfig {
            endpoint: "ws://127.0.0.1/stream".into(),
            token: "000000".into(),
            source_id: "test:color-bars".into(),
            region_x: 0,
            region_y: 0,
            region_width: 0,
            region_height: 0,
            fps: 30,
            jpeg_quality: 80,
            video_transport: "tcp".into(),
            display_orientation: DisplayOrientation::Portrait,
            portal_restore_token: None,
        }
    }

    #[test]
    fn static_color_bars_use_full_device_canvas() {
        let image =
            static_color_bars(u32::from(protocol::WIDTH), u32::from(protocol::HEIGHT)).to_rgb8();
        assert_eq!(
            image.dimensions(),
            (u32::from(protocol::WIDTH), u32::from(protocol::HEIGHT))
        );
        let expected = [
            [255, 255, 255],
            [255, 255, 0],
            [0, 255, 255],
            [0, 255, 0],
            [255, 0, 255],
            [255, 0, 0],
            [0, 0, 255],
            [0, 0, 0],
        ];
        for (index, color) in expected.into_iter().enumerate() {
            let x = ((index * 2 + 1) * image.width() as usize / (expected.len() * 2)) as u32;
            assert_eq!(image.get_pixel(x, image.height() / 2).0, color);
        }
    }

    #[test]
    fn static_color_bars_build_a_valid_cdp1_jpeg_frame() {
        let (frame, jpeg_bytes) = build_frame(
            static_color_bars(u32::from(protocol::WIDTH), u32::from(protocol::HEIGHT)),
            &test_config(),
            42,
        )
        .unwrap();
        assert_eq!(jpeg_bytes, frame.len() - protocol::HEADER_LEN);
        assert!(jpeg_bytes <= protocol::MAX_JPEG_BYTES);
        assert_eq!(&frame[..4], b"CDP1");
        assert_eq!(u32::from_le_bytes(frame[8..12].try_into().unwrap()), 42);
        assert_eq!(
            &frame[protocol::HEADER_LEN..protocol::HEADER_LEN + 2],
            [0xff, 0xd8]
        );
    }

    #[test]
    fn all_orientations_keep_the_native_wire_dimensions() {
        for orientation in [
            DisplayOrientation::Portrait,
            DisplayOrientation::LandscapeLeft,
            DisplayOrientation::PortraitInverted,
            DisplayOrientation::LandscapeRight,
        ] {
            let mut config = test_config();
            config.display_orientation = orientation;
            let (logical_width, logical_height) =
                orientation.logical_size(u32::from(protocol::WIDTH), u32::from(protocol::HEIGHT));
            let (frame, _) =
                build_frame(static_color_bars(logical_width, logical_height), &config, 7).unwrap();
            assert_eq!(
                u16::from_le_bytes(frame[12..14].try_into().unwrap()),
                protocol::WIDTH
            );
            assert_eq!(
                u16::from_le_bytes(frame[14..16].try_into().unwrap()),
                protocol::HEIGHT
            );
            let jpeg = image::load_from_memory(&frame[protocol::HEADER_LEN..]).unwrap();
            assert_eq!(
                jpeg.dimensions(),
                (u32::from(protocol::WIDTH), u32::from(protocol::HEIGHT))
            );
        }
    }

    #[test]
    fn replays_cached_frame_with_a_new_sequence() {
        let frame = protocol::encode_frame(4, &[1, 2, 3, 4]).unwrap();
        let replay = replay_frame(&frame, 9).unwrap();
        assert_eq!(u32::from_le_bytes(replay[8..12].try_into().unwrap()), 9);
        assert_eq!(&replay[..8], &frame[..8]);
        assert_eq!(&replay[12..], &frame[12..]);
    }

    #[test]
    fn parses_content_length_without_waiting_for_disconnect() {
        let body = br#"{"wifi":"connected"}"#;
        let response = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            std::str::from_utf8(body).unwrap(),
        );
        let (body_start, content_length) =
            parse_http_response(response.as_bytes()).unwrap().unwrap();
        assert_eq!(content_length, body.len());
        assert_eq!(&response.as_bytes()[body_start..], body);
    }

    #[test]
    fn waits_for_complete_headers() {
        assert!(
            parse_http_response(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n")
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn rejects_invalid_status_and_missing_length() {
        assert!(
            parse_http_response(b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n").is_err()
        );
        assert!(parse_http_response(b"HTTP/1.1 200 OK\r\n\r\n").is_err());
    }

    #[test]
    fn base64_matches_reference_vectors() {
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn encode_jpeg_reports_the_quality_that_fit() {
        let (jpeg, used_quality) = encode_jpeg(
            &static_color_bars(u32::from(protocol::WIDTH), u32::from(protocol::HEIGHT)),
            80,
        )
        .unwrap();
        assert!(!jpeg.is_empty());
        assert!(jpeg.len() <= protocol::MAX_JPEG_BYTES);
        assert!((30..=80).contains(&used_quality));
    }

    #[test]
    fn active_stream_error_distinguishes_running_from_stopping() {
        assert_eq!(active_stream_error(None), None);

        let stop = Arc::new(AtomicBool::new(false));
        assert_eq!(
            active_stream_error(Some(&stop)),
            Some("a Wi-Fi stream is already running")
        );

        stop.store(true, Ordering::Relaxed);
        assert_eq!(
            active_stream_error(Some(&stop)),
            Some("previous Wi-Fi stream is still stopping")
        );
    }

    #[test]
    fn stream_config_accepts_missing_and_saved_portal_tokens() {
        let base = serde_json::json!({
            "endpoint": "ws://127.0.0.1/stream",
            "token": "000000",
            "sourceId": "linux:portal",
            "regionX": 0,
            "regionY": 0,
            "regionWidth": 0,
            "regionHeight": 0,
            "fps": 25,
            "jpegQuality": 70,
            "videoTransport": "tcp",
            "displayOrientation": "portrait"
        });
        let without: StreamConfig = serde_json::from_value(base.clone()).unwrap();
        assert_eq!(without.portal_restore_token, None);

        let mut with = base;
        with["portalRestoreToken"] = serde_json::Value::String("restore-1".into());
        let with: StreamConfig = serde_json::from_value(with).unwrap();
        assert_eq!(with.portal_restore_token.as_deref(), Some("restore-1"));
    }
}
