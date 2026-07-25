use super::stream::StreamRef;

use spa::buffer::Data;
use std::convert::TryFrom;
use std::ptr::NonNull;

pub struct Buffer<'s> {
    buf: NonNull<pw_sys::pw_buffer>,

    /// In Pipewire, buffers are owned by the stream that generated them.
    /// This reference ensures that this rule is respected.
    stream: &'s StreamRef,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VideoCrop {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl Buffer<'_> {
    pub fn video_crop(&self) -> Option<VideoCrop> {
        let buffer = unsafe { self.buf.as_ref().buffer };
        if buffer.is_null() {
            return None;
        }
        let buffer = unsafe { &*buffer };
        if buffer.metas.is_null() {
            return None;
        }
        let metas = unsafe {
            std::slice::from_raw_parts(buffer.metas, usize::try_from(buffer.n_metas).ok()?)
        };
        let meta = metas
            .iter()
            .find(|meta| meta.type_ == spa_sys::SPA_META_VideoCrop)?;
        if meta.data.is_null()
            || usize::try_from(meta.size).ok()? < std::mem::size_of::<spa_sys::spa_meta_region>()
        {
            return None;
        }
        let crop = unsafe { &*(meta.data.cast::<spa_sys::spa_meta_region>()) }.region;
        Some(VideoCrop {
            x: crop.position.x,
            y: crop.position.y,
            width: crop.size.width,
            height: crop.size.height,
        })
    }

    pub(crate) unsafe fn from_raw(
        buf: *mut pw_sys::pw_buffer,
        stream: &StreamRef,
    ) -> Option<Buffer<'_>> {
        NonNull::new(buf).map(|buf| Buffer { buf, stream })
    }

    pub fn datas_mut(&mut self) -> &mut [Data] {
        let buffer: *mut spa_sys::spa_buffer = unsafe { self.buf.as_ref().buffer };

        let slice_of_data = if !buffer.is_null()
            && unsafe { (*buffer).n_datas > 0 && !(*buffer).datas.is_null() }
        {
            unsafe {
                let datas = (*buffer).datas as *mut Data;
                std::slice::from_raw_parts_mut(datas, usize::try_from((*buffer).n_datas).unwrap())
            }
        } else {
            &mut []
        };

        slice_of_data
    }

    #[cfg(feature = "v0_3_49")]
    pub fn requested(&self) -> u64 {
        unsafe { self.buf.as_ref().requested }
    }
}

impl Drop for Buffer<'_> {
    fn drop(&mut self) {
        unsafe {
            self.stream.queue_raw_buffer(self.buf.as_ptr());
        }
    }
}
