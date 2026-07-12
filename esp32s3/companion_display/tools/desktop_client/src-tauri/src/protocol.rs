pub const MAGIC: u32 = 0x3150_4443;
pub const VERSION: u8 = 1;
pub const FORMAT_JPEG: u8 = 1;
pub const HEADER_LEN: usize = 24;
pub const WIDTH: u16 = 368;
pub const HEIGHT: u16 = 448;
pub const MAX_JPEG_BYTES: usize = 100 * 1024;

pub fn encode_frame(sequence: u32, jpeg: &[u8]) -> Result<Vec<u8>, String> {
    if jpeg.is_empty() || jpeg.len() > MAX_JPEG_BYTES {
        return Err(format!("JPEG size must be between 1 and {MAX_JPEG_BYTES} bytes"));
    }
    let mut frame = Vec::with_capacity(HEADER_LEN + jpeg.len());
    frame.extend_from_slice(&MAGIC.to_le_bytes());
    frame.extend_from_slice(&[VERSION, FORMAT_JPEG]);
    frame.extend_from_slice(&(HEADER_LEN as u16).to_le_bytes());
    frame.extend_from_slice(&sequence.to_le_bytes());
    frame.extend_from_slice(&WIDTH.to_le_bytes());
    frame.extend_from_slice(&HEIGHT.to_le_bytes());
    frame.extend_from_slice(&(jpeg.len() as u32).to_le_bytes());
    frame.extend_from_slice(&crc32fast::hash(jpeg).to_le_bytes());
    frame.extend_from_slice(jpeg);
    Ok(frame)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn header_is_wire_compatible() {
        let frame = encode_frame(7, &[1, 2, 3]).unwrap();
        assert_eq!(frame.len(), HEADER_LEN + 3);
        assert_eq!(&frame[..4], b"CDP1");
        assert_eq!(u16::from_le_bytes(frame[6..8].try_into().unwrap()), HEADER_LEN as u16);
        assert_eq!(u32::from_le_bytes(frame[20..24].try_into().unwrap()), 0x55_bc_80_1d);
    }

    #[test]
    fn rejects_oversized_jpeg() {
        assert!(encode_frame(1, &vec![0; MAX_JPEG_BYTES + 1]).is_err());
    }
}
