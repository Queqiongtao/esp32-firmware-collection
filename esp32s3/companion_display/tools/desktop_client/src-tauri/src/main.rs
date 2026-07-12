mod protocol;
#[cfg(target_os = "linux")]
mod wayland_capture;

use std::io::{Cursor, Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use image::imageops::{overlay, FilterType};
use image::{DynamicImage, ImageBuffer, Rgb};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, Manager, State};
use tungstenite::{client, Error as WebSocketError, HandshakeError, Message, WebSocket};
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

enum VideoSender {
    Tcp,
    Udp {
        socket: UdpSocket,
        parameters: UdpParameters,
        sent_packets: u64,
        sent_bytes: u64,
    },
}

#[derive(Default)]
struct StreamState {
    stop: Arc<Mutex<Option<Arc<AtomicBool>>>>,
    event: Arc<Mutex<StreamEvent>>,
}

enum CaptureSession {
    StaticColorBars(DynamicImage),
    Xcap(String),
    #[cfg(target_os = "linux")]
    PipeWire(std::sync::mpsc::Receiver<Result<DynamicImage, String>>),
}

const CONNECTION_TIMEOUT: Duration = Duration::from_secs(5);
const CONTROL_PROTOCOL_VERSION: u8 = 1;
const STATIC_COLOR_BARS_ID: &str = "test:color-bars";

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
    if wayland_session() {
        sources.push(CaptureSourceInfo {
            id: "wayland:portal".into(),
            kind: "wayland".into(),
            label: "Choose in system dialog".into(),
            width: 0,
            height: 0,
        });
        return Ok(sources);
    }
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

fn static_color_bars() -> DynamicImage {
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
    let width = u32::from(protocol::WIDTH);
    let height = u32::from(protocol::HEIGHT);
    let image = ImageBuffer::from_fn(width, height, |x, _| {
        Rgb(COLORS[(x as usize * COLORS.len()) / width as usize])
    });
    DynamicImage::ImageRgb8(image)
}

fn capture_source(source_id: &str) -> Result<DynamicImage, String> {
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
            Ok(DynamicImage::ImageRgba8(
                monitor.capture_image().map_err(xcap_error)?,
            ))
        }
        "window" => {
            let window = Window::all()
                .map_err(xcap_error)?
                .into_iter()
                .nth(index)
                .ok_or_else(|| "selected window is no longer available".to_string())?;
            Ok(DynamicImage::ImageRgba8(
                window.capture_image().map_err(xcap_error)?,
            ))
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
    let resized = cropped
        .resize(
            protocol::WIDTH as u32,
            protocol::HEIGHT as u32,
            FilterType::Triangle,
        )
        .to_rgb8();
    let mut canvas = ImageBuffer::from_pixel(
        protocol::WIDTH as u32,
        protocol::HEIGHT as u32,
        Rgb([0, 0, 0]),
    );
    let offset_x = (canvas.width() - resized.width()) / 2;
    let offset_y = (canvas.height() - resized.height()) / 2;
    overlay(
        &mut canvas,
        &resized,
        i64::from(offset_x),
        i64::from(offset_y),
    );
    Ok(DynamicImage::ImageRgb8(canvas))
}

fn encode_jpeg(image: &DynamicImage, requested_quality: u8) -> Result<Vec<u8>, String> {
    for quality in (30..=requested_quality.clamp(30, 95)).rev().step_by(5) {
        let mut output = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut Cursor::new(&mut output), quality)
            .encode_image(image)
            .map_err(|error| format!("JPEG encoding failed: {error}"))?;
        if output.len() <= protocol::MAX_JPEG_BYTES {
            return Ok(output);
        }
    }
    Err("JPEG remains larger than the device 100 KiB limit".to_string())
}

fn build_frame(
    image: DynamicImage,
    config: &StreamConfig,
    sequence: u32,
) -> Result<(Vec<u8>, usize), String> {
    let image = crop_and_letterbox(image, config)?;
    let jpeg = encode_jpeg(&image, config.jpeg_quality)?;
    let jpeg_bytes = jpeg.len();
    Ok((protocol::encode_frame(sequence, &jpeg)?, jpeg_bytes))
}

fn wayland_session() -> bool {
    std::env::var_os("XDG_SESSION_TYPE").is_some_and(|value| value.eq_ignore_ascii_case("wayland"))
        || std::env::var_os("WAYLAND_DISPLAY").is_some()
}

fn open_capture_session(
    app: &AppHandle,
    config: &StreamConfig,
    stop: Arc<AtomicBool>,
) -> Result<CaptureSession, String> {
    if config.source_id == STATIC_COLOR_BARS_ID {
        return Ok(CaptureSession::StaticColorBars(static_color_bars()));
    }
    #[cfg(target_os = "linux")]
    if wayland_session() {
        if config.source_id != "wayland:portal" {
            return Err("refresh capture sources and choose the Wayland portal source".to_string());
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
        let (node_id, fd) = tauri::async_runtime::block_on(wayland_capture::open_portal())
            .map_err(|error| format!("Screen-share request failed: {error}"))?;
        emit(
            app,
            "waiting for PipeWire frames",
            "Waiting for the shared screen",
            0,
            0,
            0,
            &config.video_transport,
        );
        return Ok(CaptureSession::PipeWire(wayland_capture::start(
            node_id, fd, stop,
        )));
    }
    Ok(CaptureSession::Xcap(config.source_id.clone()))
}

fn capture_next(session: &CaptureSession) -> Result<DynamicImage, String> {
    match session {
        CaptureSession::StaticColorBars(image) => Ok(image.clone()),
        CaptureSession::Xcap(source_id) => capture_source(source_id),
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(receiver) => match receiver.recv_timeout(Duration::from_secs(10)) {
            Ok(frame) => frame,
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => Err("PipeWire did not provide a frame within 10 seconds after screen sharing was approved".to_string()),
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => Err("PipeWire capture stopped before providing a frame".to_string()),
        },
    }
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
    let event = StreamEvent {
        state: state.into(),
        detail: detail.into(),
        fps,
        jpeg_bytes,
        dropped_frames,
        video_transport: video_transport.into(),
    };
    if let Ok(mut current) = app.state::<StreamState>().event.lock() {
        *current = event.clone();
    }
    let _ = app.emit("stream-status", event);
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
            "type": "hello", "request_id": "hello-1", "protocol_version": 1,
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
        let local_ip = control.local_addr().map_err(|error| format!("Cannot read local control address: {error}"))?.ip();
        let remote_ip = control.peer_addr().map_err(|error| format!("Cannot read device control address: {error}"))?.ip();
        let (IpAddr::V4(local_ip), IpAddr::V4(remote_ip)) = (local_ip, remote_ip) else {
            return Err("UDP video requires an IPv4 control connection".to_string());
        };
        let udp = UdpSocket::bind(SocketAddr::new(IpAddr::V4(local_ip), 0))
            .map_err(|error| format!("Cannot create UDP socket: {error}"))?;
        udp.connect(SocketAddr::new(IpAddr::V4(remote_ip), parameters.port))
            .map_err(|error| format!("Cannot connect UDP socket: {error}"))?;
        let udp_local = udp.local_addr().map_err(|error| format!("Cannot read UDP local address: {error}"))?;
        let udp_remote = udp.peer_addr().map_err(|error| format!("Cannot read UDP remote address: {error}"))?;
        eprintln!("UDP video route: TCP {} -> {}; UDP {} -> {}", control.local_addr().unwrap_or(udp_local), control.peer_addr().unwrap_or(udp_remote), udp_local, udp_remote);
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
        parameters, sent_packets, sent_bytes,
        } => {
            for datagram in protocol::encode_udp_fragments(
                &frame,
                parameters.session_id,
                parameters.session_token,
                sequence,
                parameters.max_payload,
            )? {
                let sent = udp.send(&datagram).map_err(|error| format!("UDP send failed: {error}"))?;
                if sent != datagram.len() { return Err(format!("UDP send was truncated: {sent} of {} bytes", datagram.len())); }
                *sent_packets += 1;
                *sent_bytes += sent as u64;
            }
            Ok(())
        }
    }
}

fn udp_sender_stats(sender: &VideoSender) -> (u64, u64) {
    match sender {
        VideoSender::Udp { sent_packets, sent_bytes, .. } => (*sent_packets, *sent_bytes),
        VideoSender::Tcp => (0, 0),
    }
}

fn run_stream(
    app: AppHandle,
    config: StreamConfig,
    stop: Arc<AtomicBool>,
    mut socket: WebSocket<TcpStream>,
    mut sender: VideoSender,
) {
    let video_transport = config.video_transport.as_str();
    let frame_interval = Duration::from_millis(1000 / u64::from(config.fps.clamp(1, 30)));
    let mut sequence = 0_u32;
    let mut dropped_frames = 0_u32;
    let capture = match open_capture_session(&app, &config, stop.clone()) {
        Ok(capture) => capture,
        Err(error) => {
            emit(&app, "error", error, 0, 0, 0, video_transport);
            return;
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
    let (frame, jpeg_bytes) =
        match capture_next(&capture).and_then(|image| build_frame(image, &config, sequence)) {
            Ok(frame) => frame,
            Err(error) => {
                emit(
                    &app,
                    "error",
                    format!("First screenshot failed: {error}"),
                    0,
                    0,
                    0,
                    video_transport,
                );
                return;
            }
        };
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
        emit(
            &app,
            "error",
            format!("First screenshot send failed: {error}"),
            0,
            jpeg_bytes,
            0,
            video_transport,
        );
        return;
    }
    sequence = sequence.wrapping_add(1);
    let (udp_packets, udp_bytes) = udp_sender_stats(&sender);
    emit(
        &app,
        "snapshot sent",
        if video_transport == "udp" { format!("First screenshot sent over UDP ({udp_packets} datagrams, {udp_bytes} bytes)") }
        else { "First screenshot sent over TCP".to_string() },
        0,
        jpeg_bytes,
        0,
        video_transport,
    );
    let mut next_frame = Instant::now() + frame_interval;
    while !stop.load(Ordering::Relaxed) {
        let now = Instant::now();
        if now < next_frame {
            thread::sleep(next_frame - now);
        }
        next_frame = Instant::now() + frame_interval;
        let (frame, jpeg_bytes) =
            match capture_next(&capture).and_then(|image| build_frame(image, &config, sequence)) {
                Ok(frame) => frame,
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
        if let Err(error) = send_video_frame(&mut socket, &mut sender, frame, sequence) {
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
        sequence = sequence.wrapping_add(1);
        let (udp_packets, udp_bytes) = udp_sender_stats(&sender);
        emit(
            &app,
            "streaming",
            if video_transport == "udp" { format!("Sending UDP video ({udp_packets} datagrams, {udp_bytes} bytes)") }
            else { "Sending TCP video frames".to_string() },
            config.fps.clamp(1, 30),
            jpeg_bytes,
            dropped_frames,
            video_transport,
        );
    }
    let _ = socket.send(Message::Text(
        json!({"type": "stop_stream", "request_id": "stop-1"})
            .to_string()
            .into(),
    ));
    let _ = socket.close(None);
    emit(
        &app,
        "stopped",
        "Wi-Fi stream stopped",
        0,
        0,
        dropped_frames,
        video_transport,
    );
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
    let device = probe_wifi_device_impl(&config.endpoint)?;
    if device.protocol_version != CONTROL_PROTOCOL_VERSION {
        return Err(format!(
            "Device protocol v{} is incompatible. Flash firmware with control protocol v{} before streaming.",
            device.protocol_version, CONTROL_PROTOCOL_VERSION
        ));
    }
    if !device.video_transports.iter().any(|transport| transport == &config.video_transport) {
        return Err(format!("Device does not support {} video", config.video_transport.to_uppercase()));
    }
    if device.wifi != "connected" {
        return Err("Device Wi-Fi is not connected".to_string());
    }
    if device.active_transport != "none" {
        return Err(format!("Device is busy with {} transport", device.active_transport));
    }
    let valid_source = config.source_id == STATIC_COLOR_BARS_ID
        || config.source_id == "wayland:portal"
        || config
            .source_id
            .split_once(':')
            .is_some_and(|(kind, index)| {
                matches!(kind, "monitor" | "window") && index.parse::<usize>().is_ok()
            });
    if !valid_source {
        return Err("select a capture source before starting the stream".to_string());
    }
    if state
        .stop
        .lock()
        .map_err(|_| "stream state unavailable")?
        .is_some()
    {
        return Err("a Wi-Fi stream is already running".to_string());
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
    let endpoint = format!(
        "{}{}token={}",
        config.endpoint.trim_end_matches('/'),
        if config.endpoint.contains('?') {
            "&"
        } else {
            "?"
        },
        config.token
    );
    let mut socket = connect_wifi(&endpoint)?;
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
    if guard.is_some() {
        return Err("a Wi-Fi stream is already running".to_string());
    }
    let stop = Arc::new(AtomicBool::new(false));
    *guard = Some(stop.clone());
    drop(guard);
    let controller = state.stop.clone();
    thread::spawn(move || {
        run_stream(app, config, stop, socket, sender);
        if let Ok(mut active) = controller.lock() {
            *active = None;
        }
    });
    Ok(())
}

#[tauri::command]
fn stop_wifi_stream(app: AppHandle, state: State<'_, StreamState>) -> Result<(), String> {
    if let Some(stop) = state
        .stop
        .lock()
        .map_err(|_| "stream state unavailable")?
        .take()
    {
        stop.store(true, Ordering::Relaxed);
    }
    emit(&app, "stopped", "Stream stopped", 0, 0, 0, "none");
    Ok(())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(StreamState::default())
        .invoke_handler(tauri::generate_handler![
            list_capture_sources,
            probe_wifi_device,
            get_stream_status,
            start_wifi_stream,
            stop_wifi_stream
        ])
        .run(tauri::generate_context!())
        .expect("failed to run Companion Display");
}

#[cfg(test)]
mod tests {
    use super::{build_frame, parse_http_response, static_color_bars, StreamConfig};
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
        }
    }

    #[test]
    fn static_color_bars_use_full_device_canvas() {
        let image = static_color_bars().to_rgb8();
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
        let (frame, jpeg_bytes) = build_frame(static_color_bars(), &test_config(), 42).unwrap();
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
}
