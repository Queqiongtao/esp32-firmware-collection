pub const MAGIC: u32 = 0x3150_4443;
pub const VERSION: u8 = 1;
pub const FORMAT_JPEG: u8 = 1;
pub const HEADER_LEN: usize = 24;

pub fn encode_frame(sequence: u32, jpeg: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(HEADER_LEN + jpeg.len());
    frame.extend_from_slice(&MAGIC.to_le_bytes());
    frame.extend_from_slice(&[VERSION, FORMAT_JPEG]);
    frame.extend_from_slice(&(HEADER_LEN as u16).to_le_bytes());
    frame.extend_from_slice(&sequence.to_le_bytes());
    frame.extend_from_slice(&368u16.to_le_bytes());
    frame.extend_from_slice(&448u16.to_le_bytes());
    frame.extend_from_slice(&(jpeg.len() as u32).to_le_bytes());
    frame.extend_from_slice(&crc32fast::hash(jpeg).to_le_bytes());
    frame.extend_from_slice(jpeg);
    frame
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn header_is_wire_compatible() {
        let frame = encode_frame(7, &[1, 2, 3]);
        assert_eq!(frame.len(), HEADER_LEN + 3);
        assert_eq!(&frame[..4], b"CDP1");
        assert_eq!(u16::from_le_bytes(frame[6..8].try_into().unwrap()), HEADER_LEN as u16);
    }
}
