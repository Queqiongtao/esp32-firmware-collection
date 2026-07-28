mod image_quality;
mod inject;
mod orientation;
mod protocol;
mod touch;
mod touch_mapping;
#[cfg(target_os = "linux")]
mod wayland_capture;

use std::io::{Cursor, Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use image::imageops::{overlay, FilterType};
use image::{DynamicImage, ImageBuffer, Rgb};
use image_quality::{scale_percent, ImageRect, PreparedImage, SharpenWorkspace};
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
    #[serde(default)]
    projection_mode: ProjectionMode,
    #[serde(default)]
    selection_id: Option<String>,
    #[serde(default)]
    touch_control: bool,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
enum ProjectionMode {
    #[default]
    Live,
    Region,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct StreamEvent {
    state: String,
    detail: String,
    fps: u8,
    jpeg_bytes: usize,
    jpeg_quality_used: u8,
    source_scale_percent: u16,
    dropped_frames: u32,
    video_transport: String,
    capture_encode_ms: u32,
    capture_wait_ms: u32,
    capture_process_ms: u32,
    send_ms: u32,
    jpeg_encode_attempts: u8,
    device_ms: u32,
    device_replaced_frames: u32,
    projection_mode: ProjectionMode,
}

impl Default for StreamEvent {
    fn default() -> Self {
        Self {
            state: "idle".into(),
            detail: "Disconnected".into(),
            fps: 0,
            jpeg_bytes: 0,
            jpeg_quality_used: 0,
            source_scale_percent: 0,
            dropped_frames: 0,
            video_transport: "none".into(),
            capture_encode_ms: 0,
            capture_wait_ms: 0,
            capture_process_ms: 0,
            send_ms: 0,
            jpeg_encode_attempts: 0,
            device_ms: 0,
            device_replaced_frames: 0,
            projection_mode: ProjectionMode::Live,
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
    #[serde(default)]
    touch: bool,
    #[serde(rename = "touchEventsActive", alias = "touch_events_active", default)]
    touch_events_active: bool,
    #[serde(rename = "droppedFrames", alias = "dropped_frames", default)]
    dropped_frames: u32,
    #[serde(default)]
    udp: UdpDiagnostics,
    #[serde(default)]
    performance: DevicePerformance,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
struct StreamTarget {
    width: u16,
    height: u16,
    max_frame_bytes: usize,
}

impl StreamTarget {
    fn max_jpeg_bytes(&self) -> usize {
        self.max_frame_bytes - protocol::HEADER_LEN
    }
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

#[derive(Default, Deserialize)]
struct HelloCapabilities {
    #[serde(default)]
    touch: bool,
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
    #[serde(default)]
    width: Option<u16>,
    #[serde(default)]
    height: Option<u16>,
    #[serde(default)]
    max_frame_bytes: Option<usize>,
    udp: Option<UdpParameters>,
    #[serde(default)]
    capabilities: HelloCapabilities,
    #[serde(default)]
    touch: Option<touch::TouchDescriptor>,
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
    controls: Arc<Mutex<Option<SyncSender<StreamControl>>>>,
    projection_mode: Arc<Mutex<ProjectionMode>>,
    event: Arc<Mutex<StreamEvent>>,
    live: Arc<LiveParams>,
}

#[derive(Default)]
struct RegionSelectionState {
    next_id: AtomicU64,
    pending: Mutex<Option<PendingSelection>>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RegionSelectionPreview {
    id: String,
    width: u32,
    height: u32,
    content_x: u32,
    content_y: u32,
    content_width: u32,
    content_height: u32,
    source_width: u32,
    source_height: u32,
    jpeg_base64: String,
}

#[derive(Clone, Copy, Deserialize)]
#[serde(rename_all = "camelCase")]
struct CaptureRegionConfig {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

struct PendingSelection {
    id: String,
    capture: CaptureSession,
    stop: Arc<AtomicBool>,
    source_width: u32,
    source_height: u32,
}

enum StreamControl {
    Preview(SyncSender<Result<RegionSelectionPreview, String>>),
    UpdateRegion(CaptureRegionConfig, SyncSender<Result<(), String>>),
}

enum CaptureSession {
    StaticColorBars(DynamicImage),
    #[cfg(target_os = "linux")]
    PipeWire(
        Arc<wayland_capture::LatestFrame>,
        Option<Arc<wayland_capture::PortalInput>>,
    ),
    #[cfg(not(target_os = "linux"))]
    Monitor(Monitor),
    #[cfg(not(target_os = "linux"))]
    Window(Mutex<Window>),
}

const CONNECTION_TIMEOUT: Duration = Duration::from_secs(5);
const CONTROL_PROTOCOL_VERSION: u8 = 3;
const STATIC_COLOR_BARS_ID: &str = "test:color-bars";
const MAX_TARGET_DIMENSION: u16 = 4096;
const MAX_TARGET_PIXELS: u64 = 8_294_400;
const MAX_FRAME_BYTES: usize = 8 * 1024 * 1024;
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
        width: 0,
        height: 0,
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

fn crop_and_letterbox(
    image: DynamicImage,
    config: &StreamConfig,
    target: &StreamTarget,
) -> Result<PreparedImage, String> {
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
    let physical_width = u32::from(target.width);
    let physical_height = u32::from(target.height);
    let (logical_width, logical_height) = config
        .display_orientation
        .logical_size(physical_width, physical_height);
    let resized = cropped
        .resize(logical_width, logical_height, FilterType::Triangle)
        .to_rgb8();
    let source_scale_percent =
        scale_percent(crop_width, crop_height, resized.width(), resized.height());
    let mut canvas = ImageBuffer::from_pixel(logical_width, logical_height, Rgb([0, 0, 0]));
    let offset_x = (canvas.width() - resized.width()) / 2;
    let offset_y = (canvas.height() - resized.height()) / 2;
    overlay(
        &mut canvas,
        &resized,
        i64::from(offset_x),
        i64::from(offset_y),
    );
    let content_rect = config.display_orientation.map_logical_rect_to_physical(
        ImageRect {
            x: offset_x,
            y: offset_y,
            width: resized.width(),
            height: resized.height(),
        },
        physical_width,
        physical_height,
    );
    Ok(PreparedImage {
        image: DynamicImage::ImageRgb8(config.display_orientation.orient_logical_image(&canvas)),
        source_scale_percent,
        content_rect: Some(content_rect),
        capture_process_ms: 0,
    })
}

fn encode_jpeg_at_quality(image: &DynamicImage, quality: u8) -> Result<Vec<u8>, String> {
    let mut output = Vec::new();
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut Cursor::new(&mut output), quality)
        .encode_image(image)
        .map_err(|error| format!("JPEG encoding failed: {error}"))?;
    Ok(output)
}

/* Search in coarse five-point steps, then refine only the final bracket. */
fn encode_jpeg(
    image: &DynamicImage,
    requested_quality: u8,
    max_jpeg_bytes: usize,
) -> Result<(Vec<u8>, u8, u8), String> {
    let requested_quality = requested_quality.clamp(30, 95);
    let mut quality = requested_quality;
    let mut failed_quality: Option<u8> = None;
    let mut attempts = 0_u8;
    loop {
        attempts = attempts.saturating_add(1);
        let output = encode_jpeg_at_quality(image, quality)?;
        if output.len() <= max_jpeg_bytes {
            let Some(failed_quality) = failed_quality else {
                return Ok((output, quality, attempts));
            };
            for candidate in (quality.saturating_add(1)..failed_quality).rev() {
                attempts = attempts.saturating_add(1);
                let candidate_output = encode_jpeg_at_quality(image, candidate)?;
                if candidate_output.len() <= max_jpeg_bytes {
                    return Ok((candidate_output, candidate, attempts));
                }
            }
            return Ok((output, quality, attempts));
        }
        failed_quality = Some(quality);
        if quality == 30 {
            break;
        }
        quality = quality.saturating_sub(5).max(30);
    }
    Err(format!(
        "JPEG remains larger than the negotiated {max_jpeg_bytes}-byte limit"
    ))
}

const JPEG_QUALITY_PROBE_INTERVAL: Duration = Duration::from_secs(1);
const JPEG_QUALITY_PROBE_HEADROOM_PERCENT: usize = 90;

struct AdaptiveJpegEncoder {
    requested_quality: u8,
    quality: u8,
    last_size: usize,
    last_probe: Instant,
}

impl AdaptiveJpegEncoder {
    fn new(requested_quality: u8, quality: u8, last_size: usize) -> Self {
        Self {
            requested_quality: requested_quality.clamp(30, 95),
            quality: quality.clamp(30, 95),
            last_size,
            last_probe: Instant::now(),
        }
    }

    fn set_requested_quality(&mut self, requested_quality: u8) {
        let requested_quality = requested_quality.clamp(30, 95);
        if self.requested_quality != requested_quality {
            self.requested_quality = requested_quality;
            self.quality = requested_quality;
            self.last_size = 0;
            self.last_probe = Instant::now();
        }
    }

    fn encode(
        &mut self,
        image: &DynamicImage,
        max_jpeg_bytes: usize,
    ) -> Result<(Vec<u8>, u8, u8), String> {
        let previous_quality = self.quality.min(self.requested_quality).max(30);
        let should_probe = previous_quality < self.requested_quality
            && self.last_size > 0
            && self.last_size.saturating_mul(100)
                <= max_jpeg_bytes.saturating_mul(JPEG_QUALITY_PROBE_HEADROOM_PERCENT)
            && self.last_probe.elapsed() >= JPEG_QUALITY_PROBE_INTERVAL;
        let mut quality = if should_probe {
            previous_quality
                .saturating_add(1)
                .min(self.requested_quality)
        } else {
            previous_quality
        };
        if should_probe {
            self.last_probe = Instant::now();
        }

        let mut attempts = 0_u8;
        loop {
            attempts = attempts.saturating_add(1);
            let output = encode_jpeg_at_quality(image, quality)?;
            if output.len() <= max_jpeg_bytes {
                self.quality = quality;
                self.last_size = output.len();
                return Ok((output, quality, attempts));
            }
            self.last_probe = Instant::now();
            if quality == 30 {
                break;
            }
            quality = if quality > previous_quality {
                previous_quality
            } else {
                quality.saturating_sub(5).max(30)
            };
        }
        Err(format!(
            "JPEG remains larger than the negotiated {max_jpeg_bytes}-byte limit"
        ))
    }
}

fn encode_prepared_frame(
    image: &DynamicImage,
    jpeg_quality: u8,
    sequence: u32,
    target: &StreamTarget,
) -> Result<(Vec<u8>, usize, u8, u8), String> {
    let (jpeg, used_quality, attempts) = encode_jpeg(image, jpeg_quality, target.max_jpeg_bytes())?;
    let jpeg_bytes = jpeg.len();
    Ok((
        protocol::encode_frame(
            sequence,
            target.width,
            target.height,
            target.max_jpeg_bytes(),
            &jpeg,
        )?,
        jpeg_bytes,
        used_quality,
        attempts,
    ))
}

fn encode_adaptive_prepared_frame(
    image: &DynamicImage,
    encoder: &mut AdaptiveJpegEncoder,
    sequence: u32,
    target: &StreamTarget,
) -> Result<(Vec<u8>, usize, u8, u8), String> {
    let (jpeg, used_quality, attempts) = encoder.encode(image, target.max_jpeg_bytes())?;
    let jpeg_bytes = jpeg.len();
    Ok((
        protocol::encode_frame(
            sequence,
            target.width,
            target.height,
            target.max_jpeg_bytes(),
            &jpeg,
        )?,
        jpeg_bytes,
        used_quality,
        attempts,
    ))
}

#[cfg(test)]
fn build_frame(
    image: DynamicImage,
    config: &StreamConfig,
    sequence: u32,
    target: &StreamTarget,
) -> Result<(Vec<u8>, usize), String> {
    let mut prepared = crop_and_letterbox(image, config, target)?;
    prepared.apply_quality_filter(&mut SharpenWorkspace::default());
    let (frame, jpeg_bytes, _, _) =
        encode_prepared_frame(&prepared.image, config.jpeg_quality, sequence, target)?;
    Ok((frame, jpeg_bytes))
}

fn open_capture_session(
    app: &AppHandle,
    config: &StreamConfig,
    stop: Arc<AtomicBool>,
    target: &StreamTarget,
    notify: bool,
) -> Result<CaptureSession, String> {
    if config.source_id == STATIC_COLOR_BARS_ID {
        let (width, height) = config
            .display_orientation
            .logical_size(u32::from(target.width), u32::from(target.height));
        return Ok(CaptureSession::StaticColorBars(static_color_bars(
            width, height,
        )));
    }
    #[cfg(target_os = "linux")]
    {
        if config.source_id != "linux:portal" {
            return Err("refresh capture sources and choose the Linux portal source".to_string());
        }
        if notify {
            emit(
                app,
                "requesting screen share",
                "Choose a screen or window in the system dialog",
                0,
                0,
                0,
                &config.video_transport,
            );
        }
        let selection = tauri::async_runtime::block_on(wayland_capture::open_portal(
            config.portal_restore_token.as_deref(),
            config.touch_control,
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
        if notify {
            emit(
                app,
                "waiting for PipeWire frames",
                format!("Waiting for {source_type} at {source_size}"),
                0,
                0,
                0,
                &config.video_transport,
            );
        }
        let input = selection.input.map(Arc::new);
        return Ok(CaptureSession::PipeWire(
            wayland_capture::start(
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
                    target_width: u32::from(target.width),
                    target_height: u32::from(target.height),
                    orientation: config.display_orientation,
                },
                selection.source_size,
            ),
            input,
        ));
    }
    #[cfg(not(target_os = "linux"))]
    open_xcap_source(&config.source_id)
}

fn capture_raw_next(session: &CaptureSession) -> Result<DynamicImage, String> {
    match session {
        CaptureSession::StaticColorBars(image) => Ok(image.clone()),
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(receiver, _) => {
            Ok(receiver.receive(Duration::from_secs(10))?.image)
        }
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

fn capture_next(
    session: &CaptureSession,
    config: &StreamConfig,
    target: &StreamTarget,
) -> Result<PreparedImage, String> {
    #[cfg(target_os = "linux")]
    if let CaptureSession::PipeWire(receiver, _) = session {
        return receiver.receive(Duration::from_secs(10));
    }
    crop_and_letterbox(capture_raw_next(session)?, config, target)
}

fn capture_next_optional(
    session: &CaptureSession,
    config: &StreamConfig,
    target: &StreamTarget,
    timeout: Duration,
) -> Result<Option<PreparedImage>, String> {
    match session {
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(receiver, _) => receiver.receive_optional(timeout),
        _ => capture_next(session, config, target).map(Some),
    }
}

fn image_fingerprint(image: &DynamicImage) -> u32 {
    if let Some(image) = image.as_rgb8() {
        return crc32fast::hash(image.as_raw());
    }
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
        video_transport,
        FrameMetrics {
            fps,
            jpeg_bytes,
            dropped_frames,
            ..FrameMetrics::default()
        },
    );
}

#[derive(Default)]
struct FrameMetrics {
    fps: u8,
    jpeg_bytes: usize,
    jpeg_quality_used: u8,
    source_scale_percent: u16,
    dropped_frames: u32,
    capture_encode_ms: u32,
    capture_wait_ms: u32,
    capture_process_ms: u32,
    send_ms: u32,
    jpeg_encode_attempts: u8,
    device_ms: u32,
    device_replaced_frames: u32,
}

impl StreamEvent {
    fn from_metrics(
        state: impl Into<String>,
        detail: impl Into<String>,
        video_transport: &str,
        projection_mode: ProjectionMode,
        metrics: FrameMetrics,
    ) -> Self {
        Self {
            state: state.into(),
            detail: detail.into(),
            fps: metrics.fps,
            jpeg_bytes: metrics.jpeg_bytes,
            jpeg_quality_used: metrics.jpeg_quality_used,
            source_scale_percent: metrics.source_scale_percent,
            dropped_frames: metrics.dropped_frames,
            video_transport: video_transport.into(),
            capture_encode_ms: metrics.capture_encode_ms,
            capture_wait_ms: metrics.capture_wait_ms,
            capture_process_ms: metrics.capture_process_ms,
            send_ms: metrics.send_ms,
            jpeg_encode_attempts: metrics.jpeg_encode_attempts,
            device_ms: metrics.device_ms,
            device_replaced_frames: metrics.device_replaced_frames,
            projection_mode,
        }
    }
}

fn emit_metrics(
    app: &AppHandle,
    state: &str,
    detail: impl Into<String>,
    video_transport: &str,
    metrics: FrameMetrics,
) {
    let projection_mode = app
        .state::<StreamState>()
        .projection_mode
        .lock()
        .map(|mode| *mode)
        .unwrap_or_default();
    let event = StreamEvent::from_metrics(state, detail, video_transport, projection_mode, metrics);
    if let Ok(mut current) = app.state::<StreamState>().event.lock() {
        *current = event.clone();
    }
    let _ = app.emit("stream-status", event);
}

#[cfg(target_os = "linux")]
struct PortalInjector {
    input: Arc<wayland_capture::PortalInput>,
}

#[cfg(target_os = "linux")]
impl inject::InputInjector for PortalInjector {
    fn backend_name(&self) -> &'static str {
        "portal"
    }

    fn pointer_move(&mut self, x: f64, y: f64) -> Result<(), String> {
        tauri::async_runtime::block_on(self.input.pointer_move(x, y))
    }

    fn button(&mut self, button: touch::MouseButton, down: bool) -> Result<(), String> {
        let code = match button {
            touch::MouseButton::Left => wayland_capture::BTN_LEFT,
            touch::MouseButton::Right => wayland_capture::BTN_RIGHT,
        };
        tauri::async_runtime::block_on(self.input.button(code, down))
    }

    fn wheel(&mut self, steps: i32) -> Result<(), String> {
        /* Positive gesture steps mean the finger moved down; natural
        scrolling sends the wheel the other way. */
        tauri::async_runtime::block_on(self.input.wheel(-steps))
    }
}

fn create_touch_injector(capture: &CaptureSession) -> Box<dyn inject::InputInjector> {
    match capture {
        CaptureSession::StaticColorBars(_) => Box::new(inject::LogInjector),
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(_, Some(input)) => Box::new(PortalInjector {
            input: input.clone(),
        }),
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(_, None) => {
            eprintln!("Touch control: remote input portal unavailable; logging actions only");
            Box::new(inject::LogInjector)
        }
        #[cfg(not(target_os = "linux"))]
        CaptureSession::Monitor(_) | CaptureSession::Window(_) => inject::create_enigo_injector(),
    }
}

fn initial_touch_geometry(
    capture: &CaptureSession,
    config: &StreamConfig,
    target: &StreamTarget,
) -> inject::TouchGeometry {
    let mut geometry = inject::TouchGeometry {
        params: touch_mapping::MappingParams {
            panel_width: u32::from(target.width),
            panel_height: u32::from(target.height),
            orientation: config.display_orientation,
            region_x: config.region_x,
            region_y: config.region_y,
            region_width: config.region_width,
            region_height: config.region_height,
            source_width: 0,
            source_height: 0,
        },
        origin_x: 0.0,
        origin_y: 0.0,
        scale: 1.0,
    };
    match capture {
        CaptureSession::StaticColorBars(image) => {
            geometry.params.source_width = image.width();
            geometry.params.source_height = image.height();
        }
        /* PipeWire geometry is refreshed per frame from the capture layout. */
        #[cfg(target_os = "linux")]
        CaptureSession::PipeWire(..) => {}
        #[cfg(not(target_os = "linux"))]
        CaptureSession::Monitor(monitor) => {
            geometry.params.source_width = monitor.width();
            geometry.params.source_height = monitor.height();
            geometry.origin_x = f64::from(monitor.x());
            geometry.origin_y = f64::from(monitor.y());
        }
        #[cfg(not(target_os = "linux"))]
        CaptureSession::Window(window) => {
            if let Ok(window) = window.lock() {
                geometry.params.source_width = window.width();
                geometry.params.source_height = window.height();
                geometry.origin_x = f64::from(window.x());
                geometry.origin_y = f64::from(window.y());
            }
        }
    }
    geometry
}

#[cfg(target_os = "linux")]
fn refresh_touch_geometry(capture: &CaptureSession, geometry: &inject::SharedTouchGeometry) {
    let CaptureSession::PipeWire(receiver, _) = capture else {
        return;
    };
    let Some(layout) = receiver.capture_layout() else {
        return;
    };
    let Ok(mut guard) = geometry.lock() else {
        return;
    };
    /* The published crop already composes window content detection with the
    projected region, so the mapping treats it as the full source and the
    crop origin becomes the stream-space offset. */
    guard.params.region_x = 0;
    guard.params.region_y = 0;
    guard.params.region_width = 0;
    guard.params.region_height = 0;
    guard.params.source_width = layout.crop_width;
    guard.params.source_height = layout.crop_height;
    guard.origin_x = f64::from(layout.crop_x);
    guard.origin_y = f64::from(layout.crop_y);
    if let Some((logical_width, _)) = receiver.portal_logical() {
        if layout.frame_width > 0 {
            guard.scale = f64::from(logical_width) / f64::from(layout.frame_width);
        }
    }
}

fn spawn_touch_worker(
    rx: Receiver<touch::TouchBatch>,
    geometry: inject::SharedTouchGeometry,
    mut injector: Box<dyn inject::InputInjector>,
    descriptor: touch::TouchDescriptor,
    target_width: u16,
    target_height: u16,
) {
    thread::spawn(move || {
        let mut engine = touch::GestureEngine::new(target_width, target_height);
        let mut expected_seq: Option<u32> = None;
        /* Device timestamp of the last event plus its arrival time, used to
        estimate the device clock for time-based gestures (long press). */
        let mut device_clock: Option<(u32, Instant)> = None;
        eprintln!(
            "Touch control worker started ({}; touch={}x{}@{}Hz video={}x{})",
            injector.backend_name(),
            descriptor.width,
            descriptor.height,
            descriptor.max_move_hz,
            target_width,
            target_height
        );
        loop {
            match rx.recv_timeout(Duration::from_millis(50)) {
                Ok(batch) => {
                    if let Some(expected) = expected_seq {
                        if batch.seq != expected {
                            eprintln!(
                                "Touch events skipped a batch: expected {expected}, got {}",
                                batch.seq
                            );
                        }
                    }
                    expected_seq = Some(batch.seq.wrapping_add(1));
                    for raw_event in &batch.events {
                        let event = raw_event.normalize(descriptor, target_width, target_height);
                        device_clock = Some((event.t, Instant::now()));
                        let actions = engine.handle(&event);
                        inject::dispatch_actions(&actions, &geometry, injector.as_mut());
                    }
                }
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    if let Some((t, at)) = device_clock {
                        let estimated = t.wrapping_add(at.elapsed().as_millis() as u32);
                        let actions = engine.tick(estimated);
                        inject::dispatch_actions(&actions, &geometry, injector.as_mut());
                    }
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        }
    });
}

/* Forwards a device touch_events message to the injection worker; returns
true when the text was consumed as touch traffic. */
fn forward_touch_message(text: &str, touch_tx: Option<&mpsc::Sender<touch::TouchBatch>>) -> bool {
    if !text.contains("\"touch_events\"") {
        return false;
    }
    let Ok(batch) = serde_json::from_str::<touch::TouchBatch>(text) else {
        return false;
    };
    if batch.kind != "touch_events" {
        return false;
    }
    if let Some(tx) = touch_tx {
        let _ = tx.send(batch);
    }
    true
}

fn wait_for_presented(
    socket: &mut WebSocket<TcpStream>,
    sequence: u32,
    stop: &AtomicBool,
    touch_tx: Option<&mpsc::Sender<touch::TouchBatch>>,
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
                if forward_touch_message(&text, touch_tx) {
                    continue;
                }
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
    touch_tx: Option<&mpsc::Sender<touch::TouchBatch>>,
) -> Result<(), String> {
    loop {
        match socket.read() {
            Ok(Message::Text(text)) => {
                if forward_touch_message(&text, touch_tx) {
                    continue;
                }
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
    expected_target: &StreamTarget,
    expected_touch: Option<touch::TouchDescriptor>,
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
            let (sender, target, touch_descriptor) =
                negotiate_stream(&mut socket, "udp", config.touch_control)?;
            ensure_reconnect_target(expected_target, &target)?;
            ensure_reconnect_touch(expected_touch, touch_descriptor)?;
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

fn ensure_reconnect_target(expected: &StreamTarget, actual: &StreamTarget) -> Result<(), String> {
    if actual == expected {
        return Ok(());
    }
    Err(format!(
        "Device stream target changed from {}x{} / {} bytes to {}x{} / {} bytes; restart the stream",
        expected.width,
        expected.height,
        expected.max_frame_bytes,
        actual.width,
        actual.height,
        actual.max_frame_bytes
    ))
}

fn ensure_reconnect_touch(
    expected: Option<touch::TouchDescriptor>,
    actual: Option<touch::TouchDescriptor>,
) -> Result<(), String> {
    if actual == expected {
        return Ok(());
    }
    Err(format!(
        "Device touch description changed from {expected:?} to {actual:?}; restart the stream"
    ))
}

fn ensure_status_dimensions(
    status_width: u16,
    status_height: u16,
    target: &StreamTarget,
) -> Result<(), String> {
    if status_width == target.width && status_height == target.height {
        return Ok(());
    }
    Err(format!(
        "Device status reports {status_width}x{status_height}, but hello_ack reports {}x{}",
        target.width, target.height
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

fn validate_stream_target(
    hello: &ControlReply,
    requested_transport: &str,
) -> Result<StreamTarget, String> {
    let width = hello
        .width
        .ok_or_else(|| "Device hello_ack has no target width".to_string())?;
    let height = hello
        .height
        .ok_or_else(|| "Device hello_ack has no target height".to_string())?;
    let advertised_frame_bytes = hello
        .max_frame_bytes
        .ok_or_else(|| "Device hello_ack has no frame limit".to_string())?;
    if width == 0 || height == 0 || width > MAX_TARGET_DIMENSION || height > MAX_TARGET_DIMENSION {
        return Err(format!(
            "Device provided invalid target dimensions {width}x{height}"
        ));
    }
    if u64::from(width) * u64::from(height) > MAX_TARGET_PIXELS {
        return Err(format!(
            "Device target {width}x{height} exceeds the client pixel limit"
        ));
    }
    if !(protocol::HEADER_LEN + 1..=MAX_FRAME_BYTES).contains(&advertised_frame_bytes) {
        return Err(format!(
            "Device provided invalid max_frame_bytes {advertised_frame_bytes}"
        ));
    }
    let max_frame_bytes = if requested_transport == "udp" {
        let udp = hello
            .udp
            .as_ref()
            .ok_or_else(|| "Device did not provide UDP session parameters".to_string())?;
        if udp.max_payload == 0 || udp.max_payload > 1200 {
            return Err("Device provided an invalid UDP payload limit".to_string());
        }
        advertised_frame_bytes.min(udp.max_payload * protocol::MAX_UDP_FRAGMENTS)
    } else {
        advertised_frame_bytes
    };
    if max_frame_bytes <= protocol::HEADER_LEN {
        return Err("Negotiated frame limit cannot contain a JPEG payload".to_string());
    }
    Ok(StreamTarget {
        width,
        height,
        max_frame_bytes,
    })
}

fn validate_touch_descriptor(
    hello: &ControlReply,
    subscribe_touch: bool,
) -> Result<Option<touch::TouchDescriptor>, String> {
    if !hello.capabilities.touch {
        if hello.touch.is_some() {
            return Err("Device provided touch parameters without touch capability".to_string());
        }
        return Ok(None);
    }
    if !subscribe_touch {
        return Err("Device enabled touch events that the client did not subscribe to".to_string());
    }
    let descriptor = hello
        .touch
        .ok_or_else(|| "Device enabled touch without a touch description".to_string())?;
    if descriptor.width < 2
        || descriptor.height < 2
        || descriptor.width > MAX_TARGET_DIMENSION
        || descriptor.height > MAX_TARGET_DIMENSION
        || u64::from(descriptor.width) * u64::from(descriptor.height) > MAX_TARGET_PIXELS
        || !(1..=1000).contains(&descriptor.max_move_hz)
    {
        return Err(format!(
            "Device provided invalid touch parameters {descriptor:?}"
        ));
    }
    Ok(Some(descriptor))
}

fn negotiate_transport(
    socket: &mut WebSocket<TcpStream>,
    requested_transport: &str,
    subscribe_touch: bool,
) -> Result<(VideoSender, StreamTarget, Option<touch::TouchDescriptor>), String> {
    let hello = control_request(
        socket,
        json!({
            "type": "hello", "request_id": "hello-1", "protocol_version": CONTROL_PROTOCOL_VERSION,
            "video_transport": requested_transport, "features": {"touch_events": subscribe_touch}
        }),
        "hello-1",
    )?;
    if hello.kind != "hello_ack" || hello.video_transport != requested_transport {
        return Err("Device did not confirm the requested video transport".to_string());
    }
    let target = validate_stream_target(&hello, requested_transport)?;
    let touch_descriptor = validate_touch_descriptor(&hello, subscribe_touch)?;
    let sender = if requested_transport == "udp" {
        let parameters = hello
            .udp
            .ok_or_else(|| "Device did not provide UDP session parameters".to_string())?;
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
    Ok((sender, target, touch_descriptor))
}

fn start_device_stream(
    socket: &mut WebSocket<TcpStream>,
    requested_transport: &str,
) -> Result<(), String> {
    let started = control_request(
        socket,
        json!({"type": "start_stream", "request_id": "start-1"}),
        "start-1",
    )?;
    if started.kind != "stream_started" || started.video_transport != requested_transport {
        return Err("Device did not start the requested video transport".to_string());
    }
    Ok(())
}

fn negotiate_stream(
    socket: &mut WebSocket<TcpStream>,
    requested_transport: &str,
    subscribe_touch: bool,
) -> Result<(VideoSender, StreamTarget, Option<touch::TouchDescriptor>), String> {
    let negotiated = negotiate_transport(socket, requested_transport, subscribe_touch)?;
    start_device_stream(socket, requested_transport)?;
    Ok(negotiated)
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
    mut config: StreamConfig,
    target: StreamTarget,
    stop: Arc<AtomicBool>,
    live: Arc<LiveParams>,
    mut socket: WebSocket<TcpStream>,
    mut sender: VideoSender,
    capture_override: Option<CaptureSession>,
    controls: Receiver<StreamControl>,
    touch_descriptor: Option<touch::TouchDescriptor>,
) -> u32 {
    let video_transport = config.video_transport.as_str();
    let mut fps = config.fps.clamp(1, 30);
    let mut jpeg_quality = config.jpeg_quality.clamp(30, 95);
    live.fps.store(fps, Ordering::Relaxed);
    live.quality.store(jpeg_quality, Ordering::Relaxed);
    let mut frame_interval = Duration::from_millis(1000 / u64::from(fps));
    let mut dropped_frames = 0_u32;
    let mut heartbeat = None;
    let mut sequence = 0_u32;
    let capture = match capture_override.map_or_else(
        || open_capture_session(&app, &config, stop.clone(), &target, true),
        Ok,
    ) {
        Ok(capture) => capture,
        Err(error) => {
            emit(&app, "error", error, 0, 0, 0, video_transport);
            close_device_stream(&mut socket);
            return dropped_frames;
        }
    };
    let mut sharpen_workspace = SharpenWorkspace::default();
    #[cfg(target_os = "linux")]
    if config.projection_mode == ProjectionMode::Region {
        if let CaptureSession::PipeWire(receiver, _) = &capture {
            if let Err(error) = receiver.set_transform(wayland_capture::CaptureTransform {
                region_x: config.region_x,
                region_y: config.region_y,
                region_width: config.region_width,
                region_height: config.region_height,
                target_width: u32::from(target.width),
                target_height: u32::from(target.height),
                orientation: config.display_orientation,
            }) {
                emit(&app, "error", error, 0, 0, 0, video_transport);
                close_device_stream(&mut socket);
                return dropped_frames;
            }
        }
    }
    let touch_geometry: inject::SharedTouchGeometry = Arc::new(Mutex::new(initial_touch_geometry(
        &capture, &config, &target,
    )));
    let touch_tx = if let Some(descriptor) = touch_descriptor {
        let (tx, rx) = mpsc::channel();
        spawn_touch_worker(
            rx,
            touch_geometry.clone(),
            create_touch_injector(&capture),
            descriptor,
            target.width,
            target.height,
        );
        Some(tx)
    } else {
        None
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
    let prepared = match capture_next(&capture, &config, &target) {
        Ok(prepared) => prepared,
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
    let first_capture_process_ms = prepared.capture_process_ms;
    let encode_started = Instant::now();
    let first_encoded = {
        let fingerprint = image_fingerprint(&prepared.image);
        let mut prepared = prepared;
        prepared.apply_quality_filter(&mut sharpen_workspace);
        encode_prepared_frame(&prepared.image, jpeg_quality, sequence, &target).map(
            |(frame, jpeg_bytes, used_quality, attempts)| {
                (
                    frame,
                    jpeg_bytes,
                    used_quality,
                    attempts,
                    fingerprint,
                    prepared.source_scale_percent,
                )
            },
        )
    };
    let (
        frame,
        jpeg_bytes,
        first_fingerprint,
        mut last_used_quality,
        mut source_scale_percent,
        first_jpeg_encode_attempts,
    ): (Vec<u8>, usize, u32, u8, u16, u8) = match first_encoded {
        Ok((frame, jpeg_bytes, used_quality, attempts, fingerprint, scale_percent)) => (
            frame,
            jpeg_bytes,
            fingerprint,
            used_quality,
            scale_percent,
            attempts,
        ),
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
    let mut adaptive_encoder =
        AdaptiveJpegEncoder::new(jpeg_quality, last_used_quality, jpeg_bytes);
    let first_capture_encode_ms = encode_started.elapsed().as_millis() as u32;
    emit(
        &app,
        "starting device stream",
        "First frame is ready; starting the device video session",
        0,
        jpeg_bytes,
        0,
        video_transport,
    );
    if let Err(error) = start_device_stream(&mut socket, video_transport) {
        if !stop.load(Ordering::Relaxed) {
            emit(
                &app,
                "error",
                format!("Device stream start failed: {error}"),
                0,
                jpeg_bytes,
                0,
                video_transport,
            );
        }
        close_device_stream(&mut socket);
        return dropped_frames;
    }
    if matches!(sender, VideoSender::Udp { .. }) {
        if let Err(error) = configure_udp_control_socket(&mut socket) {
            emit(&app, "error", error, 0, jpeg_bytes, 0, video_transport);
            close_device_stream(&mut socket);
            return dropped_frames;
        }
        heartbeat = Some(UdpControlHeartbeat::new());
    }
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
    let send_started = Instant::now();
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
    let first_send_ms = send_started.elapsed().as_millis() as u32;
    let mut last_video_sent = Instant::now();
    let mut static_frames = 0_u32;
    let mut device_ms = 0;
    let mut device_replaced_frames = 0;
    if matches!(sender, VideoSender::Tcp) {
        match wait_for_presented(&mut socket, sequence, &stop, touch_tx.as_ref()) {
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
        "frame ready",
        if video_transport == "udp" {
            format!("First screenshot sent over UDP ({udp_packets} datagrams, {udp_bytes} bytes)")
        } else {
            "First screenshot sent over TCP".to_string()
        },
        video_transport,
        FrameMetrics {
            jpeg_bytes,
            jpeg_quality_used: last_used_quality,
            source_scale_percent,
            capture_encode_ms: first_capture_encode_ms,
            capture_wait_ms: first_capture_wait_ms,
            capture_process_ms: first_capture_process_ms,
            send_ms: first_send_ms,
            jpeg_encode_attempts: first_jpeg_encode_attempts,
            device_ms,
            device_replaced_frames,
            ..FrameMetrics::default()
        },
    );
    let mut next_frame = Instant::now() + frame_interval;
    let mut selection_size = (
        config.region_x.saturating_add(config.region_width),
        config.region_y.saturating_add(config.region_height),
    );
    while !stop.load(Ordering::Relaxed) {
        while let Ok(command) = controls.try_recv() {
            match command {
                StreamControl::Preview(reply) => {
                    let id = format!("active-region-{sequence}");
                    let result = capture_selection_frame(&capture).and_then(|frame| {
                        selection_size = (frame.source_width, frame.source_height);
                        encode_selection_preview(id, frame)
                    });
                    let _ = reply.send(result);
                }
                StreamControl::UpdateRegion(region, reply) => {
                    let result = validate_region(region, selection_size.0, selection_size.1)
                        .and_then(|_| {
                            config.region_x = region.x;
                            config.region_y = region.y;
                            config.region_width = region.width;
                            config.region_height = region.height;
                            #[cfg(target_os = "linux")]
                            if let CaptureSession::PipeWire(receiver, _) = &capture {
                                receiver.set_transform(wayland_capture::CaptureTransform {
                                    region_x: region.x,
                                    region_y: region.y,
                                    region_width: region.width,
                                    region_height: region.height,
                                    target_width: u32::from(target.width),
                                    target_height: u32::from(target.height),
                                    orientation: config.display_orientation,
                                })?;
                            }
                            if let Ok(mut guard) = touch_geometry.lock() {
                                guard.params.region_x = region.x;
                                guard.params.region_y = region.y;
                                guard.params.region_width = region.width;
                                guard.params.region_height = region.height;
                            }
                            last_fingerprint = last_fingerprint.wrapping_add(1);
                            Ok(())
                        });
                    let _ = reply.send(result);
                }
            }
        }
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
            adaptive_encoder.set_requested_quality(jpeg_quality);
            /* Force a re-encode so the new quality applies to a static scene too. */
            last_fingerprint = last_fingerprint.wrapping_add(1);
        }
        #[cfg(target_os = "linux")]
        refresh_touch_geometry(&capture, &touch_geometry);
        if let Some(heartbeat) = heartbeat.as_mut() {
            if let Err(reason) = service_udp_control(&mut socket, heartbeat, touch_tx.as_ref()) {
                match reconnect_udp_control(
                    &app,
                    &config,
                    &stop,
                    &reason,
                    dropped_frames,
                    &target,
                    touch_descriptor,
                ) {
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
        let image = match capture_next_optional(&capture, &config, &target, frame_interval) {
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
        let capture_process_ms = image
            .as_ref()
            .map_or(0, |prepared| prepared.capture_process_ms);
        /* Window sources can resize; keep the touch mapping in step with the
        actual source dimensions. PipeWire refreshes from its layout instead. */
        #[cfg(not(target_os = "linux"))]
        if let Some(image) = image.as_ref() {
            if let Ok(mut guard) = touch_geometry.lock() {
                guard.params.source_width = image.width();
                guard.params.source_height = image.height();
            }
        }
        let encode_started = Instant::now();
        /* Fingerprint the already cropped/scaled target image exactly once
        per captured frame; a raw desktop-sized hash costs far more. */
        let prepared = match image {
            Some(prepared) => {
                let fingerprint = image_fingerprint(&prepared.image);
                (fingerprint != last_fingerprint).then_some((prepared, fingerprint))
            }
            None => None,
        };
        let static_frame = prepared.is_none();
        let (frame, jpeg_bytes, jpeg_encode_attempts): (Vec<u8>, usize, u8) = match prepared {
            Some((mut prepared, fingerprint)) => {
                prepared.apply_quality_filter(&mut sharpen_workspace);
                match encode_adaptive_prepared_frame(
                    &prepared.image,
                    &mut adaptive_encoder,
                    sequence,
                    &target,
                ) {
                    Ok((frame, jpeg_bytes, used_quality, attempts)) => {
                        cached_frame = frame.clone();
                        last_fingerprint = fingerprint;
                        last_used_quality = used_quality;
                        source_scale_percent = prepared.source_scale_percent;
                        static_frames = 0;
                        (frame, jpeg_bytes, attempts)
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
                    Ok(frame) => (frame, cached_frame.len() - protocol::HEADER_LEN, 0),
                    Err(error) => {
                        emit(&app, "error", error, 0, 0, dropped_frames, video_transport);
                        break;
                    }
                }
            }
        };
        let capture_encode_ms = encode_started.elapsed().as_millis() as u32;
        emit_preview(&app, &frame, &mut last_preview);
        let send_started = Instant::now();
        if let Err(error) = send_video_frame(&mut socket, &mut sender, frame, sequence) {
            if matches!(sender, VideoSender::Udp { .. }) {
                match reconnect_udp_control(
                    &app,
                    &config,
                    &stop,
                    &error,
                    dropped_frames,
                    &target,
                    touch_descriptor,
                ) {
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
        let send_ms = send_started.elapsed().as_millis() as u32;
        if matches!(sender, VideoSender::Tcp) {
            match wait_for_presented(&mut socket, sequence, &stop, touch_tx.as_ref()) {
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
                            video_transport,
                            FrameMetrics {
                                jpeg_bytes,
                                jpeg_quality_used: last_used_quality,
                                source_scale_percent,
                                dropped_frames,
                                capture_encode_ms,
                                capture_wait_ms,
                                capture_process_ms,
                                send_ms,
                                jpeg_encode_attempts,
                                device_ms,
                                device_replaced_frames,
                                ..FrameMetrics::default()
                            },
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
            video_transport,
            FrameMetrics {
                fps,
                jpeg_bytes,
                jpeg_quality_used: last_used_quality,
                source_scale_percent,
                dropped_frames,
                capture_encode_ms,
                capture_wait_ms,
                capture_process_ms,
                send_ms,
                jpeg_encode_attempts,
                device_ms,
                device_replaced_frames,
            },
        );
    }
    close_device_stream(&mut socket);
    dropped_frames
}

struct RawSelectionFrame {
    image: DynamicImage,
    content_x: u32,
    content_y: u32,
    content_width: u32,
    content_height: u32,
    source_width: u32,
    source_height: u32,
}

fn capture_selection_frame(capture: &CaptureSession) -> Result<RawSelectionFrame, String> {
    #[cfg(target_os = "linux")]
    if let CaptureSession::PipeWire(receiver, _) = capture {
        let frame = receiver.request_preview(Duration::from_secs(10))?;
        return Ok(RawSelectionFrame {
            image: frame.image,
            content_x: frame.content_x,
            content_y: frame.content_y,
            content_width: frame.content_width,
            content_height: frame.content_height,
            source_width: frame.source_width,
            source_height: frame.source_height,
        });
    }
    let image = capture_raw_next(capture)?;
    let width = image.width();
    let height = image.height();
    Ok(RawSelectionFrame {
        image,
        content_x: 0,
        content_y: 0,
        content_width: width,
        content_height: height,
        source_width: width,
        source_height: height,
    })
}

fn encode_selection_preview(
    id: String,
    frame: RawSelectionFrame,
) -> Result<RegionSelectionPreview, String> {
    let (jpeg, _, _) = encode_jpeg(&frame.image, 80, 8 * 1024 * 1024)?;
    Ok(RegionSelectionPreview {
        id,
        width: frame.image.width(),
        height: frame.image.height(),
        content_x: frame.content_x,
        content_y: frame.content_y,
        content_width: frame.content_width,
        content_height: frame.content_height,
        source_width: frame.source_width,
        source_height: frame.source_height,
        jpeg_base64: base64_encode(&jpeg),
    })
}

#[tauri::command]
fn prepare_region_selection(
    app: AppHandle,
    mut config: StreamConfig,
    target_width: u16,
    target_height: u16,
    state: State<'_, RegionSelectionState>,
) -> Result<RegionSelectionPreview, String> {
    if config.source_id == STATIC_COLOR_BARS_ID {
        return Err("select a screen or window for region projection".to_string());
    }
    config.region_x = 0;
    config.region_y = 0;
    config.region_width = 0;
    config.region_height = 0;
    let target = StreamTarget {
        width: target_width,
        height: target_height,
        max_frame_bytes: MAX_FRAME_BYTES,
    };
    let stop = Arc::new(AtomicBool::new(false));
    let capture = open_capture_session(&app, &config, stop.clone(), &target, false)?;
    let id = format!(
        "region-{}",
        state.next_id.fetch_add(1, Ordering::Relaxed) + 1
    );
    let raw = capture_selection_frame(&capture)?;
    let source_width = raw.source_width;
    let source_height = raw.source_height;
    let preview = encode_selection_preview(id.clone(), raw)?;
    let mut pending = state
        .pending
        .lock()
        .map_err(|_| "region selection state unavailable")?;
    if let Some(old) = pending.replace(PendingSelection {
        id,
        capture,
        stop,
        source_width,
        source_height,
    }) {
        old.stop.store(true, Ordering::Relaxed);
    }
    Ok(preview)
}

#[tauri::command]
fn cancel_region_selection(
    selection_id: String,
    state: State<'_, RegionSelectionState>,
) -> Result<(), String> {
    let mut pending = state
        .pending
        .lock()
        .map_err(|_| "region selection state unavailable")?;
    if pending
        .as_ref()
        .is_some_and(|selection| selection.id == selection_id)
    {
        if let Some(selection) = pending.take() {
            selection.stop.store(true, Ordering::Relaxed);
        }
    }
    Ok(())
}

#[tauri::command]
fn prepare_region_reselection(
    stream: State<'_, StreamState>,
) -> Result<RegionSelectionPreview, String> {
    let sender = stream
        .controls
        .lock()
        .map_err(|_| "stream state unavailable")?
        .clone()
        .ok_or_else(|| "no region projection is active".to_string())?;
    let (tx, rx) = mpsc::sync_channel(1);
    sender
        .try_send(StreamControl::Preview(tx))
        .map_err(|_| "region selection is busy".to_string())?;
    rx.recv().map_err(|_| "region stream stopped".to_string())?
}

#[tauri::command]
fn replace_stream_region(
    region: CaptureRegionConfig,
    stream: State<'_, StreamState>,
) -> Result<(), String> {
    validate_region(region, u32::MAX, u32::MAX)?;
    let sender = stream
        .controls
        .lock()
        .map_err(|_| "stream state unavailable")?
        .clone()
        .ok_or_else(|| "no region projection is active".to_string())?;
    let (tx, rx) = mpsc::sync_channel(1);
    sender
        .try_send(StreamControl::UpdateRegion(region, tx))
        .map_err(|_| "region update is busy".to_string())?;
    rx.recv().map_err(|_| "region stream stopped".to_string())?
}

fn validate_region(region: CaptureRegionConfig, width: u32, height: u32) -> Result<(), String> {
    if region.width < 16
        || region.height < 16
        || region.x >= width
        || region.y >= height
        || region.width > width - region.x
        || region.height > height - region.y
    {
        return Err(
            "selected region is outside the capture source or smaller than 16x16".to_string(),
        );
    }
    Ok(())
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
    selections: State<'_, RegionSelectionState>,
) -> Result<StreamTarget, String> {
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
    let (sender, target, touch_descriptor) =
        negotiate_transport(&mut socket, &config.video_transport, config.touch_control)?;
    if config.touch_control && touch_descriptor.is_none() {
        emit(
            &app,
            "negotiating",
            "Device declined touch control; continuing without it",
            0,
            0,
            0,
            &config.video_transport,
        );
    }
    if let Err(error) = ensure_status_dimensions(device.width, device.height, &target) {
        close_device_stream(&mut socket);
        return Err(error);
    }
    let (capture_override, prepared_stop) = if config.projection_mode == ProjectionMode::Region {
        let id = config
            .selection_id
            .as_deref()
            .ok_or_else(|| "select a capture region before starting".to_string())?;
        let mut pending = selections
            .pending
            .lock()
            .map_err(|_| "region selection state unavailable")?;
        let selection = pending
            .take()
            .filter(|selection| selection.id == id)
            .ok_or_else(|| "region selection expired or was already used".to_string())?;
        if let Err(error) = validate_region(
            CaptureRegionConfig {
                x: config.region_x,
                y: config.region_y,
                width: config.region_width,
                height: config.region_height,
            },
            selection.source_width,
            selection.source_height,
        ) {
            selection.stop.store(true, Ordering::Relaxed);
            close_device_stream(&mut socket);
            return Err(error);
        }
        (Some(selection.capture), Some(selection.stop))
    } else {
        (None, None)
    };
    let mut guard = state.stop.lock().map_err(|_| "stream state unavailable")?;
    if let Some(error) = active_stream_error(guard.as_ref()) {
        close_device_stream(&mut socket);
        return Err(error.to_string());
    }
    let stop = prepared_stop.unwrap_or_else(|| Arc::new(AtomicBool::new(false)));
    *guard = Some(stop.clone());
    drop(guard);
    if let Ok(mut mode) = state.projection_mode.lock() {
        *mode = config.projection_mode;
    }
    let (control_tx, control_rx) = mpsc::sync_channel(1);
    if config.projection_mode == ProjectionMode::Region {
        *state
            .controls
            .lock()
            .map_err(|_| "stream state unavailable")? = Some(control_tx);
    }
    let controller = state.stop.clone();
    let control_state = state.controls.clone();
    let live = state.live.clone();
    let final_app = app.clone();
    let final_transport = config.video_transport.clone();
    let final_stop = stop.clone();
    let stream_target = target.clone();
    thread::spawn(move || {
        let dropped_frames = run_stream(
            app,
            config,
            stream_target,
            stop,
            live,
            socket,
            sender,
            capture_override,
            control_rx,
            touch_descriptor,
        );
        if let Ok(mut active) = controller.lock() {
            *active = None;
        }
        if let Ok(mut controls) = control_state.lock() {
            *controls = None;
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
    Ok(target)
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
        .manage(RegionSelectionState::default())
        .invoke_handler(tauri::generate_handler![
            list_capture_sources,
            prepare_region_selection,
            cancel_region_selection,
            prepare_region_reselection,
            replace_stream_region,
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
    use image::{DynamicImage, GenericImageView, ImageBuffer, Rgb};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    use super::{
        active_stream_error, base64_encode, build_frame, encode_jpeg, encode_jpeg_at_quality,
        ensure_reconnect_target, ensure_reconnect_touch, ensure_status_dimensions,
        parse_http_response, replay_frame, static_color_bars, validate_region,
        validate_stream_target, validate_touch_descriptor, AdaptiveJpegEncoder,
        CaptureRegionConfig, ControlReply, DisplayOrientation, FrameMetrics, ProjectionMode,
        StreamConfig, StreamEvent, StreamTarget, UdpParameters,
    };
    use crate::{protocol, touch};

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
            projection_mode: ProjectionMode::Live,
            selection_id: None,
            touch_control: false,
        }
    }

    fn s3_target() -> StreamTarget {
        StreamTarget {
            width: 368,
            height: 448,
            max_frame_bytes: 100 * 1024 + protocol::HEADER_LEN,
        }
    }

    fn p4_target() -> StreamTarget {
        StreamTarget {
            width: 1024,
            height: 600,
            max_frame_bytes: 384 * 1024 + protocol::HEADER_LEN,
        }
    }

    #[test]
    fn static_color_bars_use_full_device_canvas() {
        let target = p4_target();
        let image = static_color_bars(u32::from(target.width), u32::from(target.height)).to_rgb8();
        assert_eq!(image.dimensions(), (1024, 600));
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
        let target = s3_target();
        let (frame, jpeg_bytes) = build_frame(
            static_color_bars(u32::from(target.width), u32::from(target.height)),
            &test_config(),
            42,
            &target,
        )
        .unwrap();
        assert_eq!(jpeg_bytes, frame.len() - protocol::HEADER_LEN);
        assert!(jpeg_bytes <= target.max_jpeg_bytes());
        assert_eq!(&frame[..4], b"CDP1");
        assert_eq!(u32::from_le_bytes(frame[8..12].try_into().unwrap()), 42);
        assert_eq!(
            &frame[protocol::HEADER_LEN..protocol::HEADER_LEN + 2],
            [0xff, 0xd8]
        );
    }

    #[test]
    fn all_orientations_keep_the_native_wire_dimensions() {
        let target = p4_target();
        for orientation in [
            DisplayOrientation::Portrait,
            DisplayOrientation::LandscapeLeft,
            DisplayOrientation::PortraitInverted,
            DisplayOrientation::LandscapeRight,
        ] {
            let mut config = test_config();
            config.display_orientation = orientation;
            let (logical_width, logical_height) =
                orientation.logical_size(u32::from(target.width), u32::from(target.height));
            let (frame, _) = build_frame(
                static_color_bars(logical_width, logical_height),
                &config,
                7,
                &target,
            )
            .unwrap();
            assert_eq!(
                u16::from_le_bytes(frame[12..14].try_into().unwrap()),
                target.width
            );
            assert_eq!(
                u16::from_le_bytes(frame[14..16].try_into().unwrap()),
                target.height
            );
            let jpeg = image::load_from_memory(&frame[protocol::HEADER_LEN..]).unwrap();
            assert_eq!(
                jpeg.dimensions(),
                (u32::from(target.width), u32::from(target.height))
            );
        }
    }

    #[test]
    fn replays_cached_frame_with_a_new_sequence() {
        let frame = protocol::encode_frame(4, 368, 448, 16, &[1, 2, 3, 4]).unwrap();
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
        let target = s3_target();
        let (jpeg, used_quality, attempts) = encode_jpeg(
            &static_color_bars(u32::from(target.width), u32::from(target.height)),
            80,
            target.max_jpeg_bytes(),
        )
        .unwrap();
        assert!(!jpeg.is_empty());
        assert!(jpeg.len() <= target.max_jpeg_bytes());
        assert!((30..=80).contains(&used_quality));
        assert!(attempts >= 1);
    }

    #[test]
    fn encode_jpeg_refines_to_the_highest_quality_within_limit() {
        let image = DynamicImage::ImageRgb8(ImageBuffer::from_fn(160, 90, |x, y| {
            Rgb([
                ((x * 13 + y * 3) & 0xff) as u8,
                ((x * 5 + y * 17) & 0xff) as u8,
                ((x * 11 + y * 7) & 0xff) as u8,
            ])
        }));
        let sizes: Vec<(u8, usize)> = (30..=85)
            .map(|quality| {
                (
                    quality,
                    encode_jpeg_at_quality(&image, quality).unwrap().len(),
                )
            })
            .collect();
        let limit = sizes.iter().find(|(quality, _)| *quality == 72).unwrap().1;
        let expected = sizes
            .iter()
            .filter(|(_, size)| *size <= limit)
            .map(|(quality, _)| *quality)
            .max()
            .unwrap();
        let (_, used_quality, _) = encode_jpeg(&image, 85, limit).unwrap();
        assert_eq!(used_quality, expected);
    }

    #[test]
    fn adaptive_jpeg_uses_one_pass_at_a_stable_quality() {
        let image = static_color_bars(160, 90);
        let initial = encode_jpeg_at_quality(&image, 75).unwrap();
        let mut encoder = AdaptiveJpegEncoder::new(85, 75, initial.len());
        let (first, quality, attempts) = encoder.encode(&image, 8 * 1024 * 1024).unwrap();
        assert_eq!(quality, 75);
        assert_eq!(attempts, 1);
        let (second, quality, attempts) = encoder.encode(&image, 8 * 1024 * 1024).unwrap();
        assert_eq!(quality, 75);
        assert_eq!(attempts, 1);
        assert_eq!(first, second);
    }

    #[test]
    fn adaptive_jpeg_probes_up_at_most_once_per_interval() {
        let image = static_color_bars(160, 90);
        let mut encoder = AdaptiveJpegEncoder::new(80, 70, 1);
        encoder.last_probe = Instant::now() - Duration::from_secs(2);
        let (_, quality, attempts) = encoder.encode(&image, 8 * 1024 * 1024).unwrap();
        assert_eq!(quality, 71);
        assert_eq!(attempts, 1);
        let (_, quality, attempts) = encoder.encode(&image, 8 * 1024 * 1024).unwrap();
        assert_eq!(quality, 71);
        assert_eq!(attempts, 1);
    }

    #[test]
    fn adaptive_jpeg_falls_back_only_when_the_frame_is_too_large() {
        let image = DynamicImage::ImageRgb8(ImageBuffer::from_fn(160, 90, |x, y| {
            Rgb([
                ((x * 13 + y * 3) & 0xff) as u8,
                ((x * 5 + y * 17) & 0xff) as u8,
                ((x * 11 + y * 7) & 0xff) as u8,
            ])
        }));
        let limit = encode_jpeg_at_quality(&image, 60).unwrap().len();
        let mut encoder = AdaptiveJpegEncoder::new(80, 80, 0);
        let (jpeg, quality, attempts) = encoder.encode(&image, limit).unwrap();
        assert!(jpeg.len() <= limit);
        assert!(quality <= 60);
        assert!(attempts > 1);
        let (_, repeated_quality, repeated_attempts) = encoder.encode(&image, limit).unwrap();
        assert_eq!(repeated_quality, quality);
        assert_eq!(repeated_attempts, 1);
    }

    #[test]
    fn adaptive_jpeg_applies_live_quality_changes() {
        let image = static_color_bars(160, 90);
        let mut encoder = AdaptiveJpegEncoder::new(85, 75, 1);
        encoder.set_requested_quality(60);
        let (_, quality, attempts) = encoder.encode(&image, 8 * 1024 * 1024).unwrap();
        assert_eq!((quality, attempts), (60, 1));

        encoder.set_requested_quality(80);
        let (_, quality, attempts) = encoder.encode(&image, 8 * 1024 * 1024).unwrap();
        assert_eq!((quality, attempts), (80, 1));
    }

    #[test]
    fn adaptive_jpeg_probe_failure_returns_to_the_previous_quality() {
        let image = DynamicImage::ImageRgb8(ImageBuffer::from_fn(160, 90, |x, y| {
            Rgb([
                ((x * 13 + y * 3) & 0xff) as u8,
                ((x * 5 + y * 17) & 0xff) as u8,
                ((x * 11 + y * 7) & 0xff) as u8,
            ])
        }));
        let sizes = (30..=80)
            .map(|quality| {
                (
                    quality,
                    encode_jpeg_at_quality(&image, quality).unwrap().len(),
                )
            })
            .collect::<Vec<_>>();
        let ((quality, limit), _) = sizes
            .windows(2)
            .find_map(|pair| (pair[1].1 > pair[0].1).then_some((pair[0], pair[1])))
            .expect("test image must grow at some adjacent JPEG quality");
        let mut encoder = AdaptiveJpegEncoder::new(quality + 1, quality, 1);
        encoder.last_probe = Instant::now() - Duration::from_secs(2);
        let (jpeg, used_quality, attempts) = encoder.encode(&image, limit).unwrap();
        assert!(jpeg.len() <= limit);
        assert_eq!(used_quality, quality);
        assert_eq!(attempts, 2);
    }

    #[test]
    fn adaptive_jpeg_probes_only_with_ten_percent_headroom() {
        let image = static_color_bars(160, 90);
        let limit = 8 * 1024 * 1024;
        let mut at_threshold = AdaptiveJpegEncoder::new(80, 70, limit * 90 / 100);
        at_threshold.last_probe = Instant::now() - Duration::from_secs(2);
        let (_, quality, _) = at_threshold.encode(&image, limit).unwrap();
        assert_eq!(quality, 71);

        let mut above_threshold = AdaptiveJpegEncoder::new(80, 70, limit * 90 / 100 + 1);
        above_threshold.last_probe = Instant::now() - Duration::from_secs(2);
        let (_, quality, _) = above_threshold.encode(&image, limit).unwrap();
        assert_eq!(quality, 70);
    }

    #[test]
    fn adaptive_jpeg_rejects_a_frame_that_does_not_fit_at_quality_30() {
        let image = static_color_bars(160, 90);
        let minimum = encode_jpeg_at_quality(&image, 30).unwrap().len();
        let mut encoder = AdaptiveJpegEncoder::new(30, 30, 0);
        assert!(encoder
            .encode(&image, minimum.saturating_sub(1))
            .unwrap_err()
            .contains("remains larger"));
    }

    #[test]
    fn stream_event_metrics_use_camel_case_and_zero_for_cached_frames() {
        let event = StreamEvent::from_metrics(
            "streaming",
            "changed frame",
            "udp",
            ProjectionMode::Region,
            FrameMetrics {
                fps: 20,
                jpeg_bytes: 42_000,
                jpeg_quality_used: 75,
                source_scale_percent: 53,
                dropped_frames: 2,
                capture_encode_ms: 8,
                capture_wait_ms: 3,
                capture_process_ms: 4,
                send_ms: 12,
                jpeg_encode_attempts: 1,
                device_ms: 9,
                device_replaced_frames: 5,
            },
        );
        let value = serde_json::to_value(event).unwrap();
        assert_eq!(value["captureProcessMs"], 4);
        assert_eq!(value["captureWaitMs"], 3);
        assert_eq!(value["sendMs"], 12);
        assert_eq!(value["jpegEncodeAttempts"], 1);
        assert_eq!(value["projectionMode"], "region");
        assert!(value.get("capture_process_ms").is_none());

        let cached = StreamEvent::from_metrics(
            "streaming",
            "cached frame",
            "tcp",
            ProjectionMode::Live,
            FrameMetrics::default(),
        );
        assert_eq!(
            serde_json::to_value(cached).unwrap()["jpegEncodeAttempts"],
            0
        );
    }

    fn hello_reply(width: u16, height: u16, max_frame_bytes: usize) -> ControlReply {
        ControlReply {
            kind: "hello_ack".into(),
            request_id: "hello-1".into(),
            code: String::new(),
            video_transport: "tcp".into(),
            width: Some(width),
            height: Some(height),
            max_frame_bytes: Some(max_frame_bytes),
            udp: None,
            capabilities: Default::default(),
            touch: None,
        }
    }

    #[test]
    fn validates_dynamic_tcp_and_udp_targets() {
        let tcp = hello_reply(1024, 600, 384 * 1024 + protocol::HEADER_LEN);
        assert_eq!(validate_stream_target(&tcp, "tcp").unwrap(), p4_target());

        let mut udp = hello_reply(1024, 600, 1200 * protocol::MAX_UDP_FRAGMENTS);
        udp.video_transport = "udp".into();
        udp.udp = Some(UdpParameters {
            port: 5000,
            session_id: 1,
            session_token: 2,
            max_payload: 1200,
        });
        assert_eq!(
            validate_stream_target(&udp, "udp").unwrap().max_frame_bytes,
            1200 * protocol::MAX_UDP_FRAGMENTS
        );
    }

    #[test]
    fn rejects_unsafe_or_incomplete_targets() {
        assert!(validate_stream_target(&hello_reply(0, 600, 1024), "tcp").is_err());
        assert!(validate_stream_target(&hello_reply(4096, 4096, 1024), "tcp").is_err());
        assert!(
            validate_stream_target(&hello_reply(1024, 600, protocol::HEADER_LEN), "tcp").is_err()
        );
        let mut missing = hello_reply(1024, 600, 1024);
        missing.width = None;
        assert!(validate_stream_target(&missing, "tcp").is_err());
    }

    #[test]
    fn rejects_status_and_reconnect_target_changes() {
        let target = p4_target();
        assert!(ensure_status_dimensions(1024, 600, &target).is_ok());
        assert!(ensure_status_dimensions(368, 448, &target).is_err());
        assert!(ensure_reconnect_target(&target, &target).is_ok());
        assert!(ensure_reconnect_target(&target, &s3_target()).is_err());
        let touch = touch::TouchDescriptor {
            width: 1024,
            height: 600,
            max_move_hz: 60,
        };
        assert!(ensure_reconnect_touch(Some(touch), Some(touch)).is_ok());
        assert!(ensure_reconnect_touch(Some(touch), None).is_err());
    }

    #[test]
    fn validates_conditional_touch_description() {
        let mut hello = hello_reply(1024, 600, 512 * 1024);
        assert_eq!(validate_touch_descriptor(&hello, true).unwrap(), None);

        hello.capabilities.touch = true;
        assert!(validate_touch_descriptor(&hello, true).is_err());
        hello.touch = Some(touch::TouchDescriptor {
            width: 800,
            height: 480,
            max_move_hz: 60,
        });
        assert_eq!(
            validate_touch_descriptor(&hello, true).unwrap(),
            hello.touch
        );
        assert!(validate_touch_descriptor(&hello, false).is_err());

        hello.capabilities.touch = false;
        assert!(validate_touch_descriptor(&hello, true).is_err());
    }

    #[test]
    fn parses_touch_description_from_hello_ack() {
        let hello: ControlReply = serde_json::from_value(serde_json::json!({
            "type": "hello_ack",
            "request_id": "hello-1",
            "video_transport": "tcp",
            "width": 1024,
            "height": 600,
            "max_frame_bytes": 393240,
            "capabilities": {"touch": true},
            "touch": {"width": 800, "height": 480, "max_move_hz": 60}
        }))
        .unwrap();
        assert_eq!(
            validate_touch_descriptor(&hello, true).unwrap(),
            Some(touch::TouchDescriptor {
                width: 800,
                height: 480,
                max_move_hz: 60,
            })
        );
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
        assert_eq!(without.projection_mode, ProjectionMode::Live);

        let mut with = base.clone();
        with["portalRestoreToken"] = serde_json::Value::String("restore-1".into());
        let with: StreamConfig = serde_json::from_value(with).unwrap();
        assert_eq!(with.portal_restore_token.as_deref(), Some("restore-1"));

        let mut region = base;
        region["projectionMode"] = serde_json::Value::String("region".into());
        let region: StreamConfig = serde_json::from_value(region).unwrap();
        assert_eq!(region.projection_mode, ProjectionMode::Region);
    }

    #[test]
    fn region_validation_rejects_small_and_out_of_bounds_rectangles() {
        assert!(validate_region(
            CaptureRegionConfig {
                x: 10,
                y: 20,
                width: 100,
                height: 80
            },
            200,
            150
        )
        .is_ok());
        assert!(validate_region(
            CaptureRegionConfig {
                x: 10,
                y: 20,
                width: 8,
                height: 80
            },
            200,
            150
        )
        .is_err());
        assert!(validate_region(
            CaptureRegionConfig {
                x: 150,
                y: 20,
                width: 100,
                height: 80
            },
            200,
            150
        )
        .is_err());
    }
}
