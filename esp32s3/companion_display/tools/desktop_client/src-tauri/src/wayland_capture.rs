use std::io::Cursor;
use std::os::fd::OwnedFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use std::sync::Arc;

use ashpd::desktop::{
    screencast::{CursorMode, Screencast, SourceType, Stream as PortalStream},
    PersistMode,
};
use image::{DynamicImage, RgbaImage};
use pipewire as pw;
use pw::{properties::properties, spa};

struct PipeWireData {
    format: spa::param::video::VideoInfoRaw,
    frames: SyncSender<Result<DynamicImage, String>>,
    stop: Arc<AtomicBool>,
    mainloop: pw::main_loop::MainLoop,
}

pub async fn open_portal() -> Result<(u32, OwnedFd), String> {
    let portal = Screencast::new().await.map_err(|error| error.to_string())?;
    let session = portal.create_session().await.map_err(|error| error.to_string())?;
    portal
        .select_sources(
            &session,
            CursorMode::Hidden,
            (SourceType::Monitor | SourceType::Window).into(),
            false,
            None,
            PersistMode::DoNot,
        )
        .await
        .map_err(|error| error.to_string())?;
    let response = portal.start(&session, None).await
        .map_err(|error| error.to_string())?
        .response()
        .map_err(|error| error.to_string())?;
    let stream: PortalStream = response
        .streams()
        .first()
        .ok_or_else(|| "no screen or window was selected".to_string())?
        .to_owned();
    let remote = portal.open_pipe_wire_remote(&session).await.map_err(|error| error.to_string())?;
    Ok((stream.pipe_wire_node_id(), remote))
}

pub fn start(node_id: u32, remote: OwnedFd, stop: Arc<AtomicBool>) -> Receiver<Result<DynamicImage, String>> {
    let (frames, receiver) = sync_channel(1);
    std::thread::spawn(move || {
        if let Err(error) = run(node_id, remote, stop, frames.clone()) {
            let _ = frames.try_send(Err(error));
        }
    });
    receiver
}

fn run(node_id: u32, remote: OwnedFd, stop: Arc<AtomicBool>, frames: SyncSender<Result<DynamicImage, String>>) -> Result<(), String> {
    pw::init();
    let mainloop = pw::main_loop::MainLoop::new(None).map_err(|error| format!("PipeWire main loop failed: {error}"))?;
    let context = pw::context::Context::new(&mainloop).map_err(|error| format!("PipeWire context failed: {error}"))?;
    let core = context.connect_fd(remote, None).map_err(|error| format!("PipeWire connection failed: {error}"))?;
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
            stop,
            mainloop: mainloop.clone(),
        })
        .state_changed(|_, user_data, _, state| {
            if matches!(state, pw::stream::StreamState::Unconnected) {
                user_data.mainloop.quit();
            }
        })
        .param_changed(|_, user_data, id, param| {
            let Some(param) = param else { return; };
            if id != spa::param::ParamType::Format.as_raw() {
                return;
            }
            if let Ok((media_type, media_subtype)) = spa::param::format_utils::parse_format(param) {
                if media_type == spa::param::format::MediaType::Video && media_subtype == spa::param::format::MediaSubtype::Raw {
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
            let Some(mut buffer) = stream.dequeue_buffer() else { return; };
            let Some(data) = buffer.datas_mut().first_mut() else { return; };
            if let Ok(image) = image_from_data(data, &user_data.format) {
                let _ = user_data.frames.try_send(Ok(image));
            }
        })
        .register()
        .map_err(|error| format!("PipeWire listener registration failed: {error}"))?;

    let format = pw::spa::pod::object!(
        pw::spa::utils::SpaTypes::ObjectParamFormat,
        pw::spa::param::ParamType::EnumFormat,
        pw::spa::pod::property!(pw::spa::param::format::FormatProperties::MediaType, Id, pw::spa::param::format::MediaType::Video),
        pw::spa::pod::property!(pw::spa::param::format::FormatProperties::MediaSubtype, Id, pw::spa::param::format::MediaSubtype::Raw),
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
            pw::spa::utils::Rectangle { width: 1920, height: 1080 },
            pw::spa::utils::Rectangle { width: 1, height: 1 },
            pw::spa::utils::Rectangle { width: 4096, height: 4096 }
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::VideoFramerate,
            Choice,
            Range,
            Fraction,
            pw::spa::utils::Fraction { num: 30, denom: 1 },
            pw::spa::utils::Fraction { num: 0, denom: 1 },
            pw::spa::utils::Fraction { num: 1000, denom: 1 }
        ),
    );
    let values = pw::spa::pod::serialize::PodSerializer::serialize(
        Cursor::new(Vec::new()),
        &pw::spa::pod::Value::Object(format),
    )
    .map_err(|error| format!("PipeWire format serialization failed: {error}"))?
    .0
    .into_inner();
    let mut params = [spa::pod::Pod::from_bytes(&values).ok_or_else(|| "PipeWire format creation failed".to_string())?];
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

fn image_from_data(data: &mut spa::buffer::Data, format: &spa::param::video::VideoInfoRaw) -> Result<DynamicImage, String> {
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
    let bytes = data.data().ok_or_else(|| "PipeWire frame is not mapped into memory".to_string())?;
    let needed = offset + stride.saturating_mul(height.saturating_sub(1)) + width * 4;
    if needed > bytes.len() || chunk_size + offset < needed {
        return Err("PipeWire frame has an invalid buffer layout".to_string());
    }
    let mut rgba = Vec::with_capacity(width * height * 4);
    for row in 0..height {
        let source = &bytes[offset + row * stride..offset + row * stride + width * 4];
        for pixel in source.chunks_exact(4) {
            match pixel_format {
                value if value == spa::param::video::VideoFormat::RGBx || value == spa::param::video::VideoFormat::RGBA => {
                    rgba.extend_from_slice(&[pixel[0], pixel[1], pixel[2], 255]);
                }
                value if value == spa::param::video::VideoFormat::BGRx || value == spa::param::video::VideoFormat::BGRA => {
                    rgba.extend_from_slice(&[pixel[2], pixel[1], pixel[0], 255]);
                }
                _ => return Err(format!("Unsupported PipeWire video format: {pixel_format:?}")),
            }
        }
    }
    let image = RgbaImage::from_raw(width as u32, height as u32, rgba)
        .ok_or_else(|| "PipeWire image conversion failed".to_string())?;
    Ok(DynamicImage::ImageRgba8(image))
}
