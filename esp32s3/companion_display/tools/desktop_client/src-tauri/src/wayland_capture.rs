use std::io::Cursor;
use std::os::fd::OwnedFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use ashpd::desktop::{
    screencast::{CursorMode, Screencast, SourceType, Stream as PortalStream},
    PersistMode,
};
use image::{DynamicImage, ImageBuffer, Rgb};
use pipewire as pw;
use pw::{properties::properties, spa};

struct PipeWireData {
    format: spa::param::video::VideoInfoRaw,
    frames: Arc<LatestFrame>,
    transform: CaptureTransform,
    stop: Arc<AtomicBool>,
    mainloop: pw::main_loop::MainLoop,
}

#[derive(Clone, Copy)]
pub struct CaptureTransform {
    pub region_x: u32,
    pub region_y: u32,
    pub region_width: u32,
    pub region_height: u32,
    pub target_width: u32,
    pub target_height: u32,
}

pub struct LatestFrame {
    frame: Mutex<Option<Result<DynamicImage, String>>>,
    ready: Condvar,
}

impl LatestFrame {
    fn publish(&self, frame: Result<DynamicImage, String>) {
        if let Ok(mut pending) = self.frame.lock() {
            *pending = Some(frame);
            self.ready.notify_one();
        }
    }

    pub fn receive(&self, timeout: Duration) -> Result<DynamicImage, String> {
        self.receive_optional(timeout)?.ok_or_else(|| "PipeWire did not provide a frame within 10 seconds after screen sharing was approved".to_string())
    }

    pub fn receive_optional(&self, timeout: Duration) -> Result<Option<DynamicImage>, String> {
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
}

fn require_embedded_cursor(supported: bool) -> Result<CursorMode, String> {
    supported
        .then_some(CursorMode::Embedded)
        .ok_or_else(|| "Your desktop portal cannot embed the mouse cursor. Update xdg-desktop-portal and its desktop backend before sharing.".to_string())
}

pub async fn open_portal() -> Result<(u32, OwnedFd), String> {
    let portal = Screencast::new().await.map_err(|error| error.to_string())?;
    let cursor_modes = portal
        .available_cursor_modes()
        .await
        .map_err(|error| format!("Cannot query desktop cursor support: {error}"))?;
    let cursor_mode = require_embedded_cursor(cursor_modes.contains(CursorMode::Embedded))?;
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
            None,
            PersistMode::DoNot,
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
    let remote = portal
        .open_pipe_wire_remote(&session)
        .await
        .map_err(|error| error.to_string())?;
    Ok((stream.pipe_wire_node_id(), remote))
}

pub fn start(
    node_id: u32,
    remote: OwnedFd,
    stop: Arc<AtomicBool>,
    fps: u8,
    transform: CaptureTransform,
) -> Arc<LatestFrame> {
    let frames = Arc::new(LatestFrame {
        frame: Mutex::new(None),
        ready: Condvar::new(),
    });
    let producer = frames.clone();
    std::thread::spawn(move || {
        if let Err(error) = run(node_id, remote, stop, producer.clone(), fps, transform) {
            producer.publish(Err(error));
        }
    });
    frames
}

fn run(
    node_id: u32,
    remote: OwnedFd,
    stop: Arc<AtomicBool>,
    frames: Arc<LatestFrame>,
    fps: u8,
    transform: CaptureTransform,
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
                    let _ = user_data.format.parse(param);
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
            let Some(data) = buffer.datas_mut().first_mut() else {
                return;
            };
            match image_from_data(data, &user_data.format, user_data.transform) {
                Ok(image) => user_data.frames.publish(Ok(image)),
                Err(error) => user_data.frames.publish(Err(error)),
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
            pw::spa::param::video::VideoFormat::BGRx,
            pw::spa::param::video::VideoFormat::BGRx,
            pw::spa::param::video::VideoFormat::RGBx,
            pw::spa::param::video::VideoFormat::BGRA,
            pw::spa::param::video::VideoFormat::RGBA,
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::VideoSize,
            Choice,
            Range,
            Rectangle,
            pw::spa::utils::Rectangle {
                width: 1920,
                height: 1080
            },
            pw::spa::utils::Rectangle {
                width: 1,
                height: 1
            },
            pw::spa::utils::Rectangle {
                width: 4096,
                height: 4096
            }
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
    let mut params = [spa::pod::Pod::from_bytes(&values)
        .ok_or_else(|| "PipeWire format creation failed".to_string())?];
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
) -> Result<DynamicImage, String> {
    let size = format.size();
    let width = size.width as usize;
    let height = size.height as usize;
    if width == 0 || height == 0 {
        return Err("PipeWire supplied an empty frame".to_string());
    }
    let pixel_format = format.format();
    let offset = data.chunk().offset() as usize;
    let chunk_size = data.chunk().size() as usize;
    let stride = data.chunk().stride().unsigned_abs() as usize;
    let stride = if stride == 0 { width * 4 } else { stride };
    let bytes = data
        .data()
        .ok_or_else(|| "PipeWire frame is not mapped into memory".to_string())?;
    let needed = offset + stride.saturating_mul(height.saturating_sub(1)) + width * 4;
    if needed > bytes.len() || chunk_size + offset < needed {
        return Err("PipeWire frame has an invalid buffer layout".to_string());
    }
    let rgb_ordered = matches!(
        pixel_format,
        spa::param::video::VideoFormat::RGBx | spa::param::video::VideoFormat::RGBA
    );
    let bgr_ordered = matches!(
        pixel_format,
        spa::param::video::VideoFormat::BGRx | spa::param::video::VideoFormat::BGRA
    );
    if !rgb_ordered && !bgr_ordered {
        return Err(format!(
            "Unsupported PipeWire video format: {pixel_format:?}"
        ));
    }
    let crop_x = transform.region_x as usize;
    let crop_y = transform.region_y as usize;
    let crop_width = if transform.region_width == 0 {
        width
    } else {
        transform.region_width as usize
    };
    let crop_height = if transform.region_height == 0 {
        height
    } else {
        transform.region_height as usize
    };
    if crop_x >= width
        || crop_y >= height
        || crop_width > width - crop_x
        || crop_height > height - crop_y
    {
        return Err("capture region is outside the shared screen".to_string());
    }
    let target_width = transform.target_width as usize;
    let target_height = transform.target_height as usize;
    let scale =
        (target_width as f32 / crop_width as f32).min(target_height as f32 / crop_height as f32);
    let draw_width = ((crop_width as f32 * scale).round() as usize)
        .max(1)
        .min(target_width);
    let draw_height = ((crop_height as f32 * scale).round() as usize)
        .max(1)
        .min(target_height);
    let offset_x = (target_width - draw_width) / 2;
    let offset_y = (target_height - draw_height) / 2;
    let mut output = ImageBuffer::from_pixel(
        transform.target_width,
        transform.target_height,
        Rgb([0, 0, 0]),
    );
    let sample = |x: usize, y: usize| -> [u8; 3] {
        let pixel = &bytes[offset + y * stride + x * 4..offset + y * stride + x * 4 + 4];
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
            output.put_pixel(
                (offset_x + output_x) as u32,
                (offset_y + output_y) as u32,
                Rgb(rgb),
            );
        }
    }
    Ok(DynamicImage::ImageRgb8(output))
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
        };
        let first = DynamicImage::ImageRgb8(ImageBuffer::from_pixel(1, 1, Rgb([1, 0, 0])));
        let latest = DynamicImage::ImageRgb8(ImageBuffer::from_pixel(1, 1, Rgb([2, 0, 0])));
        frames.publish(Ok(first));
        frames.publish(Ok(latest));
        assert_eq!(
            frames
                .receive(Duration::from_millis(1))
                .unwrap()
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
}
