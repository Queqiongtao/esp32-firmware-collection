use std::io::Cursor;
use std::os::fd::OwnedFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use crate::image_quality::{scale_percent, ImageRect, PreparedImage};
use crate::orientation::DisplayOrientation;
use ashpd::desktop::{
    remote_desktop::{Axis, DeviceType, KeyState, RemoteDesktop},
    screencast::{CursorMode, Screencast, SourceType, Stream as PortalStream},
    PersistMode, Session,
};
use image::{DynamicImage, ImageBuffer, Rgb};
use pipewire as pw;
use pw::{properties::properties, spa};

struct PipeWireData {
    format: spa::param::video::VideoInfoRaw,
    frames: Arc<LatestFrame>,
    transform: Arc<LatestFrame>,
    is_window: bool,
    layout_logged: bool,
    last_layout_issue: Option<FrameLayoutIssue>,
    last_alpha_status: Option<AlphaStatus>,
    stop: Arc<AtomicBool>,
    mainloop: pw::main_loop::MainLoop,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FrameLayoutIssue {
    Corrupted,
    EmptyChunk,
    InvalidStride,
    InvalidRange,
}

impl FrameLayoutIssue {
    fn description(self) -> &'static str {
        match self {
            Self::Corrupted => "buffer is marked corrupted",
            Self::EmptyChunk => "buffer contains no frame data",
            Self::InvalidStride => "stride is smaller than one pixel row",
            Self::InvalidRange => "frame data falls outside the mapped buffer",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct FrameLayout {
    first_row_offset: usize,
    stride: isize,
}

impl FrameLayout {
    fn row_offset(self, y: usize) -> usize {
        self.first_row_offset
            .checked_add_signed(self.stride.saturating_mul(y as isize))
            .expect("validated PipeWire row offset")
    }
}

enum CaptureFrameError {
    Retry(FrameLayoutIssue),
    Fatal(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct CaptureRegion {
    x: usize,
    y: usize,
    width: usize,
    height: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AlphaStatus {
    VideoCrop(CaptureRegion),
    NotUsed,
    Bounds(CaptureRegion),
    FullyOpaque,
    Empty,
}

struct PreparedFrame {
    image: DynamicImage,
    alpha_status: AlphaStatus,
    crop: CaptureRegion,
    draw_width: usize,
    draw_height: usize,
    offset_x: usize,
    offset_y: usize,
}

#[derive(Clone, Copy)]
pub struct CaptureTransform {
    pub region_x: u32,
    pub region_y: u32,
    pub region_width: u32,
    pub region_height: u32,
    pub target_width: u32,
    pub target_height: u32,
    pub orientation: DisplayOrientation,
}

pub struct PortalSelection {
    pub node_id: u32,
    pub remote: OwnedFd,
    pub source_type: Option<SourceType>,
    pub source_size: Option<(u32, u32)>,
    pub restore_token: Option<String>,
    pub input: Option<PortalInput>,
}

/* Keeps the merged RemoteDesktop+ScreenCast session alive so pointer events
can be injected into the shared stream. All notify calls are async; callers
run them on the app's async runtime. */
pub struct PortalInput {
    remote_desktop: RemoteDesktop<'static>,
    session: Session<'static, RemoteDesktop<'static>>,
    node_id: u32,
}

pub const BTN_LEFT: i32 = 0x110;
pub const BTN_RIGHT: i32 = 0x111;

impl PortalInput {
    pub async fn pointer_move(&self, x: f64, y: f64) -> Result<(), String> {
        self.remote_desktop
            .notify_pointer_motion_absolute(&self.session, self.node_id, x, y)
            .await
            .map_err(|error| error.to_string())
    }

    pub async fn button(&self, button: i32, pressed: bool) -> Result<(), String> {
        let state = if pressed {
            KeyState::Pressed
        } else {
            KeyState::Released
        };
        self.remote_desktop
            .notify_pointer_button(&self.session, button, state)
            .await
            .map_err(|error| error.to_string())
    }

    pub async fn wheel(&self, steps: i32) -> Result<(), String> {
        self.remote_desktop
            .notify_pointer_axis_discrete(&self.session, Axis::Vertical, steps)
            .await
            .map_err(|error| error.to_string())
    }
}

/* The region of the PipeWire frame that is currently scaled onto the device
canvas, in stream pixel coordinates, plus the negotiated frame size. */
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CaptureLayout {
    pub crop_x: u32,
    pub crop_y: u32,
    pub crop_width: u32,
    pub crop_height: u32,
    pub frame_width: u32,
    pub frame_height: u32,
}

pub struct LatestFrame {
    frame: Mutex<Option<Result<PreparedImage, String>>>,
    ready: Condvar,
    preview: Mutex<Option<Result<SelectionFrame, String>>>,
    preview_ready: Condvar,
    preview_requested: AtomicBool,
    transform: Mutex<CaptureTransform>,
    layout: Mutex<Option<CaptureLayout>>,
    portal_logical: Option<(u32, u32)>,
}

pub struct SelectionFrame {
    pub image: DynamicImage,
    pub content_x: u32,
    pub content_y: u32,
    pub content_width: u32,
    pub content_height: u32,
    pub source_width: u32,
    pub source_height: u32,
}

impl LatestFrame {
    fn publish(&self, frame: Result<PreparedImage, String>) {
        if let Ok(mut pending) = self.frame.lock() {
            *pending = Some(frame);
            self.ready.notify_one();
        }
    }

    pub fn receive(&self, timeout: Duration) -> Result<PreparedImage, String> {
        self.receive_optional(timeout)?.ok_or_else(|| "PipeWire did not provide a frame within 10 seconds after screen sharing was approved".to_string())
    }

    pub fn receive_optional(&self, timeout: Duration) -> Result<Option<PreparedImage>, String> {
        let pending = self
            .frame
            .lock()
            .map_err(|_| "PipeWire frame lock failed".to_string())?;
        let (mut pending, _) = self
            .ready
            .wait_timeout_while(pending, timeout, |frame| frame.is_none())
            .map_err(|_| "PipeWire frame wait failed".to_string())?;
        pending.take().transpose()
    }

    pub fn request_preview(&self, timeout: Duration) -> Result<SelectionFrame, String> {
        *self.preview.lock().map_err(|_| "preview lock failed")? = None;
        self.preview_requested.store(true, Ordering::Release);
        let pending = self.preview.lock().map_err(|_| "preview lock failed")?;
        let (mut pending, result) = self
            .preview_ready
            .wait_timeout_while(pending, timeout, |frame| frame.is_none())
            .map_err(|_| "preview wait failed")?;
        if result.timed_out() {
            return Err("PipeWire did not provide a selection preview".to_string());
        }
        pending.take().expect("preview was signalled")
    }

    pub fn set_transform(&self, transform: CaptureTransform) -> Result<(), String> {
        *self
            .transform
            .lock()
            .map_err(|_| "capture transform lock failed")? = transform;
        *self.frame.lock().map_err(|_| "capture frame lock failed")? = None;
        Ok(())
    }

    /* The crop rectangle the PipeWire thread most recently rendered from,
    published for touch coordinate mapping. */
    pub fn capture_layout(&self) -> Option<CaptureLayout> {
        self.layout.lock().ok().and_then(|layout| *layout)
    }

    /* Portal logical size of the shared stream; differs from the PipeWire
    pixel size under fractional scaling. */
    pub fn portal_logical(&self) -> Option<(u32, u32)> {
        self.portal_logical
    }

    fn publish_layout(&self, layout: CaptureLayout) {
        if let Ok(mut slot) = self.layout.lock() {
            *slot = Some(layout);
        }
    }
}

fn require_embedded_cursor(supported: bool) -> Result<CursorMode, String> {
    supported
        .then_some(CursorMode::Embedded)
        .ok_or_else(|| "Your desktop portal cannot embed the mouse cursor. Update xdg-desktop-portal and its desktop backend before sharing.".to_string())
}

fn valid_source_size(size: Option<(i32, i32)>) -> Option<(u32, u32)> {
    let (width, height) = size?;
    let width = u32::try_from(width).ok()?;
    let height = u32::try_from(height).ok()?;
    (width > 0 && height > 0).then_some((width, height))
}

pub async fn open_portal(
    restore_token: Option<&str>,
    with_input: bool,
) -> Result<PortalSelection, String> {
    let portal = Screencast::new().await.map_err(|error| error.to_string())?;
    let cursor_modes = portal
        .available_cursor_modes()
        .await
        .map_err(|error| format!("Cannot query desktop cursor support: {error}"))?;
    let cursor_mode = require_embedded_cursor(cursor_modes.contains(CursorMode::Embedded))?;
    if with_input {
        return open_portal_with_input(&portal, cursor_mode, restore_token).await;
    }
    let session = portal
        .create_session()
        .await
        .map_err(|error| error.to_string())?;
    portal
        .select_sources(
            &session,
            cursor_mode,
            (SourceType::Monitor | SourceType::Window).into(),
            false,
            restore_token,
            PersistMode::ExplicitlyRevoked,
        )
        .await
        .map_err(|error| error.to_string())?;
    let response = portal
        .start(&session, None)
        .await
        .map_err(|error| error.to_string())?
        .response()
        .map_err(|error| error.to_string())?;
    let stream: PortalStream = response
        .streams()
        .first()
        .ok_or_else(|| "no screen or window was selected".to_string())?
        .to_owned();
    let restore_token = response.restore_token().map(ToOwned::to_owned);
    let remote = portal
        .open_pipe_wire_remote(&session)
        .await
        .map_err(|error| error.to_string())?;
    Ok(PortalSelection {
        node_id: stream.pipe_wire_node_id(),
        remote,
        source_type: stream.source_type(),
        source_size: valid_source_size(stream.size()),
        restore_token,
        input: None,
    })
}

/* Touch control needs a RemoteDesktop session that also carries the screen
cast: NotifyPointerMotionAbsolute only accepts a stream of its own session.
The restore token travels through SelectDevices here, so tokens from plain
ScreenCast sessions are not interchangeable with these. */
async fn open_portal_with_input(
    portal: &Screencast<'_>,
    cursor_mode: CursorMode,
    restore_token: Option<&str>,
) -> Result<PortalSelection, String> {
    let remote_desktop = RemoteDesktop::new()
        .await
        .map_err(|error| error.to_string())?;
    let session = remote_desktop
        .create_session()
        .await
        .map_err(|error| error.to_string())?;
    remote_desktop
        .select_devices(
            &session,
            DeviceType::Pointer.into(),
            restore_token,
            PersistMode::ExplicitlyRevoked,
        )
        .await
        .map_err(|error| format!("Remote input request failed: {error}"))?;
    portal
        .select_sources(
            &session,
            cursor_mode,
            SourceType::Monitor | SourceType::Window,
            false,
            None,
            PersistMode::DoNot,
        )
        .await
        .map_err(|error| error.to_string())?;
    let response = remote_desktop
        .start(&session, None)
        .await
        .map_err(|error| error.to_string())?
        .response()
        .map_err(|error| format!("Remote control was not approved: {error}"))?;
    let stream: PortalStream = response
        .streams()
        .and_then(|streams| streams.first())
        .ok_or_else(|| "no screen or window was selected".to_string())?
        .to_owned();
    let restore_token = response.restore_token().map(ToOwned::to_owned);
    let remote = portal
        .open_pipe_wire_remote(&session)
        .await
        .map_err(|error| error.to_string())?;
    let node_id = stream.pipe_wire_node_id();
    Ok(PortalSelection {
        node_id,
        remote,
        source_type: stream.source_type(),
        source_size: valid_source_size(stream.size()),
        restore_token,
        input: Some(PortalInput {
            remote_desktop,
            session,
            node_id,
        }),
    })
}

pub fn start(
    node_id: u32,
    remote: OwnedFd,
    is_window: bool,
    stop: Arc<AtomicBool>,
    fps: u8,
    transform: CaptureTransform,
    portal_logical: Option<(u32, u32)>,
) -> Arc<LatestFrame> {
    let frames = Arc::new(LatestFrame {
        frame: Mutex::new(None),
        ready: Condvar::new(),
        preview: Mutex::new(None),
        preview_ready: Condvar::new(),
        preview_requested: AtomicBool::new(false),
        transform: Mutex::new(transform),
        layout: Mutex::new(None),
        portal_logical,
    });
    let producer = frames.clone();
    let transform_control = frames.clone();
    std::thread::spawn(move || {
        if let Err(error) = run(
            node_id,
            remote,
            is_window,
            stop,
            producer.clone(),
            fps,
            transform_control,
        ) {
            producer.publish(Err(error));
        }
    });
    frames
}

fn run(
    node_id: u32,
    remote: OwnedFd,
    is_window: bool,
    stop: Arc<AtomicBool>,
    frames: Arc<LatestFrame>,
    fps: u8,
    transform: Arc<LatestFrame>,
) -> Result<(), String> {
    pw::init();
    let mainloop = pw::main_loop::MainLoop::new(None)
        .map_err(|error| format!("PipeWire main loop failed: {error}"))?;
    let context = pw::context::Context::new(&mainloop)
        .map_err(|error| format!("PipeWire context failed: {error}"))?;
    let core = context
        .connect_fd(remote, None)
        .map_err(|error| format!("PipeWire connection failed: {error}"))?;
    let stream = pw::stream::Stream::new(
        &core,
        "companion-display-capture",
        properties! {
            *pw::keys::MEDIA_TYPE => "Video",
            *pw::keys::MEDIA_CATEGORY => "Capture",
            *pw::keys::MEDIA_ROLE => "Screen",
        },
    )
    .map_err(|error| format!("PipeWire stream creation failed: {error}"))?;

    let listener = stream
        .add_local_listener_with_user_data(PipeWireData {
            format: Default::default(),
            frames,
            transform,
            is_window,
            layout_logged: false,
            last_layout_issue: None,
            last_alpha_status: None,
            stop,
            mainloop: mainloop.clone(),
        })
        .state_changed(|_, user_data, _, state| {
            if matches!(state, pw::stream::StreamState::Unconnected) {
                user_data.mainloop.quit();
            }
        })
        .param_changed(|_, user_data, id, param| {
            let Some(param) = param else {
                return;
            };
            if id != spa::param::ParamType::Format.as_raw() {
                return;
            }
            if let Ok((media_type, media_subtype)) = spa::param::format_utils::parse_format(param) {
                if media_type == spa::param::format::MediaType::Video
                    && media_subtype == spa::param::format::MediaSubtype::Raw
                {
                    if user_data.format.parse(param).is_ok() {
                        let size = user_data.format.size();
                        eprintln!(
                            "PipeWire negotiated: size={}x{} format={:?}",
                            size.width,
                            size.height,
                            user_data.format.format()
                        );
                        user_data.layout_logged = false;
                        user_data.last_layout_issue = None;
                        user_data.last_alpha_status = None;
                    }
                }
            }
        })
        .process(|stream, user_data| {
            if user_data.stop.load(Ordering::Relaxed) {
                let _ = stream.disconnect();
                user_data.mainloop.quit();
                return;
            }
            let Some(mut buffer) = stream.dequeue_buffer() else {
                return;
            };
            let video_crop = validate_video_crop(buffer.video_crop(), &user_data.format);
            let Some(data) = buffer.datas_mut().first_mut() else {
                return;
            };
            let transform = match user_data.transform.transform.lock() {
                Ok(transform) => *transform,
                Err(_) => {
                    user_data.frames.publish(Err("capture transform lock failed".to_string()));
                    return;
                }
            };
            let wants_preview = user_data
                .transform
                .preview_requested
                .swap(false, Ordering::AcqRel);
            match image_from_data(
                data,
                &user_data.format,
                transform,
                user_data.is_window,
                video_crop,
            ) {
                Ok(Some(prepared)) => {
                    user_data.last_layout_issue = None;
                    if user_data.last_alpha_status != Some(prepared.alpha_status) {
                        match prepared.alpha_status {
                            AlphaStatus::VideoCrop(region) => eprintln!(
                                "PipeWire VideoCrop: x={} y={} width={} height={}",
                                region.x, region.y, region.width, region.height
                            ),
                            AlphaStatus::Bounds(region) => eprintln!(
                                "Window alpha bounds: x={} y={} width={} height={}",
                                region.x, region.y, region.width, region.height
                            ),
                            AlphaStatus::FullyOpaque => eprintln!(
                                "Window alpha fallback: frame is fully opaque; using the complete PipeWire frame"
                            ),
                            AlphaStatus::NotUsed => eprintln!(
                                "Window alpha fallback: negotiated format has no alpha; using the complete PipeWire frame"
                            ),
                            AlphaStatus::Empty => {}
                        }
                        user_data.last_alpha_status = Some(prepared.alpha_status);
                        user_data.layout_logged = false;
                    }
                    if !user_data.layout_logged {
                        let size = user_data.format.size();
                        eprintln!(
                            "Capture layout: source={}x{} crop={},{},{}x{} draw={}x{} offset={},{} target={}x{}",
                            size.width,
                            size.height,
                            prepared.crop.x,
                            prepared.crop.y,
                            prepared.crop.width,
                            prepared.crop.height,
                            prepared.draw_width,
                            prepared.draw_height,
                            prepared.offset_x,
                            prepared.offset_y,
                            transform.target_width,
                            transform.target_height
                        );
                    }
                    user_data.layout_logged = true;
                    {
                        let size = user_data.format.size();
                        user_data.frames.publish_layout(CaptureLayout {
                            crop_x: prepared.crop.x as u32,
                            crop_y: prepared.crop.y as u32,
                            crop_width: prepared.crop.width as u32,
                            crop_height: prepared.crop.height as u32,
                            frame_width: size.width,
                            frame_height: size.height,
                        });
                    }
                    if wants_preview {
                        let size = user_data.format.size();
                        let preview_transform = CaptureTransform {
                            region_x: 0,
                            region_y: 0,
                            region_width: 0,
                            region_height: 0,
                            target_width: size.width,
                            target_height: size.height,
                            orientation: DisplayOrientation::Portrait,
                        };
                        let preview = image_from_data(
                            data,
                            &user_data.format,
                            preview_transform,
                            user_data.is_window,
                            video_crop,
                        )
                        .map_err(|error| match error {
                            CaptureFrameError::Retry(issue) => issue.description().to_string(),
                            CaptureFrameError::Fatal(error) => error,
                        })
                        .and_then(|frame| frame.ok_or_else(|| "selection source is empty".to_string()))
                        .map(|frame| SelectionFrame {
                            image: frame.image,
                            content_x: frame.offset_x as u32,
                            content_y: frame.offset_y as u32,
                            content_width: frame.draw_width as u32,
                            content_height: frame.draw_height as u32,
                            source_width: frame.crop.width as u32,
                            source_height: frame.crop.height as u32,
                        });
                        if let Ok(mut slot) = user_data.transform.preview.lock() {
                            *slot = Some(preview);
                            user_data.transform.preview_ready.notify_one();
                        }
                    }
                    let content_rect = transform.orientation.map_logical_rect_to_physical(
                        ImageRect {
                            x: prepared.offset_x as u32,
                            y: prepared.offset_y as u32,
                            width: prepared.draw_width as u32,
                            height: prepared.draw_height as u32,
                        },
                        transform.target_width,
                        transform.target_height,
                    );
                    user_data.frames.publish(Ok(PreparedImage {
                        image: prepared.image,
                        source_scale_percent: scale_percent(
                            prepared.crop.width as u32,
                            prepared.crop.height as u32,
                            prepared.draw_width as u32,
                            prepared.draw_height as u32,
                        ),
                        content_rect: Some(content_rect),
                    }));
                }
                Ok(None) => {
                    user_data.last_layout_issue = None;
                    if user_data.last_alpha_status != Some(AlphaStatus::Empty) {
                        eprintln!("Window alpha bounds: frame is fully transparent; skipping it");
                        user_data.last_alpha_status = Some(AlphaStatus::Empty);
                    }
                }
                Err(CaptureFrameError::Retry(issue)) => {
                    if user_data.last_layout_issue != Some(issue) {
                        let chunk = data.chunk();
                        let chunk_offset = chunk.offset();
                        let chunk_size = chunk.size();
                        let chunk_stride = chunk.stride();
                        let size = user_data.format.size();
                        let mapped_len = data.data().map_or(0, |bytes| bytes.len());
                        eprintln!(
                            "Skipping transient PipeWire frame: {}; offset={} size={} stride={} mapped={} negotiated={}x{}",
                            issue.description(),
                            chunk_offset,
                            chunk_size,
                            chunk_stride,
                            mapped_len,
                            size.width,
                            size.height
                        );
                        user_data.last_layout_issue = Some(issue);
                    }
                }
                Err(CaptureFrameError::Fatal(error)) => user_data.frames.publish(Err(error)),
            }
        })
        .register()
        .map_err(|error| format!("PipeWire listener registration failed: {error}"))?;

    let format = pw::spa::pod::object!(
        pw::spa::utils::SpaTypes::ObjectParamFormat,
        pw::spa::param::ParamType::EnumFormat,
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::MediaType,
            Id,
            pw::spa::param::format::MediaType::Video
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::MediaSubtype,
            Id,
            pw::spa::param::format::MediaSubtype::Raw
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::VideoFormat,
            Choice,
            Enum,
            Id,
            pw::spa::param::video::VideoFormat::BGRA,
            pw::spa::param::video::VideoFormat::BGRA,
            pw::spa::param::video::VideoFormat::RGBA,
            pw::spa::param::video::VideoFormat::BGRx,
            pw::spa::param::video::VideoFormat::RGBx,
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::VideoFramerate,
            Choice,
            Range,
            Fraction,
            pw::spa::utils::Fraction {
                num: u32::from(fps.clamp(1, 30)),
                denom: 1
            },
            pw::spa::utils::Fraction { num: 0, denom: 1 },
            pw::spa::utils::Fraction {
                num: 1000,
                denom: 1
            }
        ),
    );
    let values = pw::spa::pod::serialize::PodSerializer::serialize(
        Cursor::new(Vec::new()),
        &pw::spa::pod::Value::Object(format),
    )
    .map_err(|error| format!("PipeWire format serialization failed: {error}"))?
    .0
    .into_inner();
    let meta = pw::spa::pod::object!(
        pw::spa::utils::SpaTypes::ObjectParamMeta,
        pw::spa::param::ParamType::Meta,
        pw::spa::pod::property!(
            pw::spa::param::MetaProperties::Type,
            Id,
            pw::spa::param::MetaType::VideoCrop
        ),
        pw::spa::pod::property!(
            pw::spa::param::MetaProperties::Size,
            Int,
            std::mem::size_of::<i32>() as i32 * 4
        ),
    );
    let meta_values = pw::spa::pod::serialize::PodSerializer::serialize(
        Cursor::new(Vec::new()),
        &pw::spa::pod::Value::Object(meta),
    )
    .map_err(|error| format!("PipeWire metadata serialization failed: {error}"))?
    .0
    .into_inner();
    let mut params = [
        spa::pod::Pod::from_bytes(&values)
            .ok_or_else(|| "PipeWire format creation failed".to_string())?,
        spa::pod::Pod::from_bytes(&meta_values)
            .ok_or_else(|| "PipeWire metadata request creation failed".to_string())?,
    ];
    stream
        .connect(
            spa::utils::Direction::Input,
            Some(node_id),
            pw::stream::StreamFlags::AUTOCONNECT | pw::stream::StreamFlags::MAP_BUFFERS,
            &mut params,
        )
        .map_err(|error| format!("PipeWire stream connection failed: {error}"))?;

    mainloop.run();
    drop(listener);
    Ok(())
}

fn image_from_data(
    data: &mut spa::buffer::Data,
    format: &spa::param::video::VideoInfoRaw,
    transform: CaptureTransform,
    is_window: bool,
    video_crop: Option<CaptureRegion>,
) -> Result<Option<PreparedFrame>, CaptureFrameError> {
    let size = format.size();
    let width = size.width as usize;
    let height = size.height as usize;
    if width == 0 || height == 0 {
        return Err(CaptureFrameError::Retry(FrameLayoutIssue::EmptyChunk));
    }
    let pixel_format = format.format();
    let chunk = data.chunk();
    let chunk_offset = chunk.offset();
    let chunk_size = chunk.size();
    let chunk_stride = chunk.stride();
    let corrupted = chunk.flags().contains(spa::buffer::ChunkFlags::CORRUPTED);
    let bytes = data.data().ok_or_else(|| {
        CaptureFrameError::Fatal("PipeWire frame is not mapped into memory".to_string())
    })?;
    let layout = frame_layout(
        width,
        height,
        chunk_offset,
        chunk_size,
        chunk_stride,
        corrupted,
        bytes.len(),
    )
    .map_err(CaptureFrameError::Retry)?;
    let rgb_ordered = matches!(
        pixel_format,
        spa::param::video::VideoFormat::RGBx | spa::param::video::VideoFormat::RGBA
    );
    let bgr_ordered = matches!(
        pixel_format,
        spa::param::video::VideoFormat::BGRx | spa::param::video::VideoFormat::BGRA
    );
    if !rgb_ordered && !bgr_ordered {
        return Err(CaptureFrameError::Fatal(format!(
            "Unsupported PipeWire video format: {pixel_format:?}"
        )));
    }
    let has_alpha = matches!(
        pixel_format,
        spa::param::video::VideoFormat::BGRA | spa::param::video::VideoFormat::RGBA
    );
    let (alpha_status, base) = if let Some(region) = video_crop {
        (AlphaStatus::VideoCrop(region), region)
    } else if is_window && has_alpha {
        match detect_alpha_region(bytes, layout, width, height) {
            AlphaStatus::Bounds(region) => (AlphaStatus::Bounds(region), region),
            AlphaStatus::FullyOpaque => (
                AlphaStatus::FullyOpaque,
                CaptureRegion {
                    x: 0,
                    y: 0,
                    width,
                    height,
                },
            ),
            AlphaStatus::Empty => return Ok(None),
            AlphaStatus::NotUsed | AlphaStatus::VideoCrop(_) => unreachable!(),
        }
    } else {
        (
            AlphaStatus::NotUsed,
            CaptureRegion {
                x: 0,
                y: 0,
                width,
                height,
            },
        )
    };
    let relative_x = transform.region_x as usize;
    let relative_y = transform.region_y as usize;
    let crop_width = if transform.region_width == 0 {
        base.width
    } else {
        transform.region_width as usize
    };
    let crop_height = if transform.region_height == 0 {
        base.height
    } else {
        transform.region_height as usize
    };
    if relative_x >= base.width
        || relative_y >= base.height
        || crop_width > base.width - relative_x
        || crop_height > base.height - relative_y
    {
        return Err(CaptureFrameError::Fatal(
            "capture region is outside the selected source".to_string(),
        ));
    }
    let crop_x = base.x + relative_x;
    let crop_y = base.y + relative_y;
    let physical_width = transform.target_width as usize;
    let physical_height = transform.target_height as usize;
    let (logical_width, logical_height) = transform
        .orientation
        .logical_size(transform.target_width, transform.target_height);
    let target_width = logical_width as usize;
    let target_height = logical_height as usize;
    let (draw_width, draw_height, offset_x, offset_y) =
        letterbox_layout(crop_width, crop_height, target_width, target_height);
    let mut output = ImageBuffer::from_pixel(
        transform.target_width,
        transform.target_height,
        Rgb([0, 0, 0]),
    );
    let sample = |x: usize, y: usize| -> [u8; 3] {
        let offset = layout.row_offset(y) + x * 4;
        let pixel = &bytes[offset..offset + 4];
        if rgb_ordered {
            [pixel[0], pixel[1], pixel[2]]
        } else {
            [pixel[2], pixel[1], pixel[0]]
        }
    };
    for output_y in 0..draw_height {
        let source_y = crop_y as f32
            + ((output_y as f32 + 0.5) * crop_height as f32 / draw_height as f32 - 0.5);
        let y0 = source_y
            .floor()
            .clamp(crop_y as f32, (crop_y + crop_height - 1) as f32) as usize;
        let y1 = (y0 + 1).min(crop_y + crop_height - 1);
        let fy = source_y - source_y.floor();
        for output_x in 0..draw_width {
            let source_x = crop_x as f32
                + ((output_x as f32 + 0.5) * crop_width as f32 / draw_width as f32 - 0.5);
            let x0 = source_x
                .floor()
                .clamp(crop_x as f32, (crop_x + crop_width - 1) as f32)
                as usize;
            let x1 = (x0 + 1).min(crop_x + crop_width - 1);
            let fx = source_x - source_x.floor();
            let p00 = sample(x0, y0);
            let p10 = sample(x1, y0);
            let p01 = sample(x0, y1);
            let p11 = sample(x1, y1);
            let mut rgb = [0_u8; 3];
            for channel in 0..3 {
                let top = p00[channel] as f32 + (p10[channel] as f32 - p00[channel] as f32) * fx;
                let bottom = p01[channel] as f32 + (p11[channel] as f32 - p01[channel] as f32) * fx;
                rgb[channel] = (top + (bottom - top) * fy).round() as u8;
            }
            let logical_x = (offset_x + output_x) as u32;
            let logical_y = (offset_y + output_y) as u32;
            let (physical_x, physical_y) = transform.orientation.map_logical_to_physical(
                logical_x,
                logical_y,
                physical_width as u32,
                physical_height as u32,
            );
            output.put_pixel(physical_x, physical_y, Rgb(rgb));
        }
    }
    Ok(Some(PreparedFrame {
        image: DynamicImage::ImageRgb8(output),
        alpha_status,
        crop: CaptureRegion {
            x: crop_x,
            y: crop_y,
            width: crop_width,
            height: crop_height,
        },
        draw_width,
        draw_height,
        offset_x,
        offset_y,
    }))
}

fn frame_layout(
    width: usize,
    height: usize,
    chunk_offset: u32,
    chunk_size: u32,
    chunk_stride: i32,
    corrupted: bool,
    mapped_len: usize,
) -> Result<FrameLayout, FrameLayoutIssue> {
    if corrupted {
        return Err(FrameLayoutIssue::Corrupted);
    }
    if chunk_size == 0 || mapped_len == 0 {
        return Err(FrameLayoutIssue::EmptyChunk);
    }
    let row_bytes = width.checked_mul(4).ok_or(FrameLayoutIssue::InvalidRange)?;
    let stride_magnitude = if chunk_stride == 0 {
        row_bytes
    } else {
        chunk_stride.unsigned_abs() as usize
    };
    if stride_magnitude < row_bytes {
        return Err(FrameLayoutIssue::InvalidStride);
    }
    let preceding_rows = stride_magnitude
        .checked_mul(height.saturating_sub(1))
        .ok_or(FrameLayoutIssue::InvalidRange)?;
    let frame_span = preceding_rows
        .checked_add(row_bytes)
        .ok_or(FrameLayoutIssue::InvalidRange)?;
    if frame_span > (chunk_size as usize).min(mapped_len) {
        return Err(FrameLayoutIssue::InvalidRange);
    }

    let first_row_offset = chunk_offset as usize % mapped_len;
    if chunk_stride < 0 {
        if first_row_offset < preceding_rows
            || first_row_offset
                .checked_add(row_bytes)
                .is_none_or(|end| end > mapped_len)
        {
            return Err(FrameLayoutIssue::InvalidRange);
        }
    } else if first_row_offset
        .checked_add(frame_span)
        .is_none_or(|end| end > mapped_len)
    {
        return Err(FrameLayoutIssue::InvalidRange);
    }

    Ok(FrameLayout {
        first_row_offset,
        stride: if chunk_stride < 0 {
            -(stride_magnitude as isize)
        } else {
            stride_magnitude as isize
        },
    })
}

fn validate_video_crop(
    crop: Option<pw::buffer::VideoCrop>,
    format: &spa::param::video::VideoInfoRaw,
) -> Option<CaptureRegion> {
    let crop = crop?;
    let x = usize::try_from(crop.x).ok()?;
    let y = usize::try_from(crop.y).ok()?;
    let width = usize::try_from(crop.width).ok()?;
    let height = usize::try_from(crop.height).ok()?;
    let size = format.size();
    let frame_width = usize::try_from(size.width).ok()?;
    let frame_height = usize::try_from(size.height).ok()?;
    (width > 0
        && height > 0
        && x < frame_width
        && y < frame_height
        && width <= frame_width - x
        && height <= frame_height - y)
        .then_some(CaptureRegion {
            x,
            y,
            width,
            height,
        })
}

fn detect_alpha_region(
    bytes: &[u8],
    layout: FrameLayout,
    width: usize,
    height: usize,
) -> AlphaStatus {
    let mut row_spans = Vec::with_capacity(height);
    let mut opaque_pixels = 0usize;
    for y in 0..height {
        let mut first = width;
        let mut last = 0usize;
        let mut count = 0usize;
        for x in 0..width {
            if bytes[layout.row_offset(y) + x * 4 + 3] != 0 {
                first = first.min(x);
                last = x;
                count += 1;
            }
        }
        opaque_pixels += count;
        row_spans.push((first, last, count));
    }
    if opaque_pixels == 0 {
        return AlphaStatus::Empty;
    }
    if opaque_pixels == width * height {
        return AlphaStatus::FullyOpaque;
    }

    let minimum_row_pixels = (width / 100).max(2);
    let mut best_start = 0usize;
    let mut best_end = 0usize;
    let mut current_start = None;
    for (y, (_, _, count)) in row_spans.iter().enumerate() {
        if *count >= minimum_row_pixels {
            current_start.get_or_insert(y);
        } else if let Some(start) = current_start.take() {
            if y - start > best_end - best_start {
                best_start = start;
                best_end = y;
            }
        }
    }
    if let Some(start) = current_start {
        if height - start > best_end - best_start {
            best_start = start;
            best_end = height;
        }
    }
    if best_end == best_start {
        return AlphaStatus::Empty;
    }

    let run_height = best_end - best_start;
    let minimum_column_pixels = (run_height / 20).max(2);
    let mut first_x = width;
    let mut last_x = 0usize;
    for x in 0..width {
        let count = (best_start..best_end)
            .filter(|y| bytes[layout.row_offset(*y) + x * 4 + 3] != 0)
            .count();
        if count >= minimum_column_pixels {
            first_x = first_x.min(x);
            last_x = x;
        }
    }
    if first_x == width {
        return AlphaStatus::Empty;
    }
    AlphaStatus::Bounds(CaptureRegion {
        x: first_x,
        y: best_start,
        width: last_x - first_x + 1,
        height: run_height,
    })
}

fn letterbox_layout(
    source_width: usize,
    source_height: usize,
    target_width: usize,
    target_height: usize,
) -> (usize, usize, usize, usize) {
    let scale = (target_width as f32 / source_width as f32)
        .min(target_height as f32 / source_height as f32);
    let draw_width = ((source_width as f32 * scale).round() as usize)
        .max(1)
        .min(target_width);
    let draw_height = ((source_height as f32 * scale).round() as usize)
        .max(1)
        .min(target_height);
    (
        draw_width,
        draw_height,
        (target_width - draw_width) / 2,
        (target_height - draw_height) / 2,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageBuffer, Rgb};

    #[test]
    fn latest_frame_replaces_stale_capture() {
        let frames = LatestFrame {
            frame: Mutex::new(None),
            ready: Condvar::new(),
            preview: Mutex::new(None),
            preview_ready: Condvar::new(),
            preview_requested: AtomicBool::new(false),
            transform: Mutex::new(CaptureTransform {
                region_x: 0,
                region_y: 0,
                region_width: 0,
                region_height: 0,
                target_width: 1,
                target_height: 1,
                orientation: DisplayOrientation::Portrait,
            }),
            layout: Mutex::new(None),
            portal_logical: None,
        };
        let first = PreparedImage {
            image: DynamicImage::ImageRgb8(ImageBuffer::from_pixel(1, 1, Rgb([1, 0, 0]))),
            source_scale_percent: 100,
            content_rect: Some(ImageRect {
                x: 0,
                y: 0,
                width: 1,
                height: 1,
            }),
        };
        let latest = PreparedImage {
            image: DynamicImage::ImageRgb8(ImageBuffer::from_pixel(1, 1, Rgb([2, 0, 0]))),
            source_scale_percent: 100,
            content_rect: Some(ImageRect {
                x: 0,
                y: 0,
                width: 1,
                height: 1,
            }),
        };
        frames.publish(Ok(first));
        frames.publish(Ok(latest));
        assert_eq!(
            frames
                .receive(Duration::from_millis(1))
                .unwrap()
                .image
                .to_rgb8()
                .get_pixel(0, 0)[0],
            2
        );
    }

    #[test]
    fn embedded_cursor_is_required_for_linux_capture() {
        assert_eq!(require_embedded_cursor(true).unwrap(), CursorMode::Embedded);
        assert!(require_embedded_cursor(false)
            .unwrap_err()
            .contains("cannot embed the mouse cursor"));
    }

    #[test]
    fn portal_source_size_must_be_positive() {
        assert_eq!(valid_source_size(Some((1600, 900))), Some((1600, 900)));
        assert_eq!(valid_source_size(None), None);
        assert_eq!(valid_source_size(Some((0, 900))), None);
        assert_eq!(valid_source_size(Some((-1, 900))), None);
    }

    #[test]
    fn letterbox_layout_preserves_wide_window() {
        assert_eq!(letterbox_layout(1600, 900, 368, 448), (368, 207, 0, 120));
    }

    #[test]
    fn letterbox_layout_preserves_tall_window() {
        assert_eq!(letterbox_layout(900, 1600, 368, 448), (252, 448, 58, 0));
    }

    #[test]
    fn letterbox_layout_fills_matching_aspect_ratio() {
        assert_eq!(letterbox_layout(736, 896, 368, 448), (368, 448, 0, 0));
    }

    fn alpha_frame(width: usize, height: usize, regions: &[CaptureRegion]) -> Vec<u8> {
        let mut bytes = vec![0_u8; width * height * 4];
        for region in regions {
            for y in region.y..region.y + region.height {
                for x in region.x..region.x + region.width {
                    bytes[(y * width + x) * 4 + 3] = 255;
                }
            }
        }
        bytes
    }

    #[test]
    fn alpha_bounds_extract_the_window_from_a_transparent_canvas() {
        let window = CaptureRegion {
            x: 2,
            y: 1,
            width: 6,
            height: 5,
        };
        let bytes = alpha_frame(10, 8, &[window]);
        assert_eq!(
            detect_alpha_region(
                &bytes,
                frame_layout(10, 8, 0, 320, 40, false, 320).unwrap(),
                10,
                8
            ),
            AlphaStatus::Bounds(window)
        );
    }

    #[test]
    fn alpha_bounds_ignore_an_isolated_cursor_outside_the_window() {
        let cursor = CaptureRegion {
            x: 0,
            y: 0,
            width: 2,
            height: 2,
        };
        let window = CaptureRegion {
            x: 4,
            y: 3,
            width: 12,
            height: 10,
        };
        let bytes = alpha_frame(20, 16, &[cursor, window]);
        assert_eq!(
            detect_alpha_region(
                &bytes,
                frame_layout(20, 16, 0, 1280, 80, false, 1280).unwrap(),
                20,
                16
            ),
            AlphaStatus::Bounds(window)
        );
    }

    #[test]
    fn alpha_bounds_report_empty_and_fully_opaque_frames() {
        let empty = alpha_frame(10, 8, &[]);
        assert_eq!(
            detect_alpha_region(
                &empty,
                frame_layout(10, 8, 0, 320, 40, false, 320).unwrap(),
                10,
                8
            ),
            AlphaStatus::Empty
        );
        let full = alpha_frame(
            10,
            8,
            &[CaptureRegion {
                x: 0,
                y: 0,
                width: 10,
                height: 8,
            }],
        );
        assert_eq!(
            detect_alpha_region(
                &full,
                frame_layout(10, 8, 0, 320, 40, false, 320).unwrap(),
                10,
                8
            ),
            AlphaStatus::FullyOpaque
        );
    }

    #[test]
    fn frame_layout_accepts_packed_padded_and_offset_frames() {
        assert_eq!(
            frame_layout(2, 2, 0, 16, 0, false, 16).unwrap(),
            FrameLayout {
                first_row_offset: 0,
                stride: 8,
            }
        );
        assert_eq!(
            frame_layout(2, 2, 0, 20, 12, false, 20).unwrap(),
            FrameLayout {
                first_row_offset: 0,
                stride: 12,
            }
        );
        assert_eq!(
            frame_layout(2, 2, 4, 16, 8, false, 20).unwrap(),
            FrameLayout {
                first_row_offset: 4,
                stride: 8,
            }
        );
    }

    #[test]
    fn frame_layout_preserves_negative_stride_orientation() {
        let layout = frame_layout(2, 2, 12, 20, -12, false, 20).unwrap();
        assert_eq!(layout.row_offset(0), 12);
        assert_eq!(layout.row_offset(1), 0);
    }

    #[test]
    fn frame_layout_classifies_transient_buffers() {
        assert_eq!(
            frame_layout(2, 2, 0, 16, 8, true, 16),
            Err(FrameLayoutIssue::Corrupted)
        );
        assert_eq!(
            frame_layout(2, 2, 0, 0, 8, false, 16),
            Err(FrameLayoutIssue::EmptyChunk)
        );
        assert_eq!(
            frame_layout(2, 2, 0, 16, 4, false, 16),
            Err(FrameLayoutIssue::InvalidStride)
        );
        assert_eq!(
            frame_layout(2, 2, 0, 15, 8, false, 16),
            Err(FrameLayoutIssue::InvalidRange)
        );
        assert_eq!(
            frame_layout(2, 2, 8, 16, 8, false, 16),
            Err(FrameLayoutIssue::InvalidRange)
        );
        assert_eq!(
            frame_layout(usize::MAX, 2, 0, u32::MAX, 0, false, usize::MAX),
            Err(FrameLayoutIssue::InvalidRange)
        );
    }
}
