pub const MAGIC: u32 = 0x3150_4443;
pub const VERSION: u8 = 1;
pub const FORMAT_JPEG: u8 = 1;
pub const HEADER_LEN: usize = 24;
pub const UDP_MAGIC: u32 = 0x3155_4443;
pub const UDP_HEADER_LEN: usize = 36;
pub const MAX_UDP_FRAGMENTS: usize = 128;

pub fn encode_frame(
    sequence: u32,
    width: u16,
    height: u16,
    max_jpeg_bytes: usize,
    jpeg: &[u8],
) -> Result<Vec<u8>, String> {
    if width == 0 || height == 0 {
        return Err("frame dimensions must be non-zero".to_string());
    }
    if jpeg.is_empty() || jpeg.len() > max_jpeg_bytes {
        return Err(format!(
            "JPEG size must be between 1 and {max_jpeg_bytes} bytes"
        ));
    }
    let mut frame = Vec::with_capacity(HEADER_LEN + jpeg.len());
    frame.extend_from_slice(&MAGIC.to_le_bytes());
    frame.extend_from_slice(&[VERSION, FORMAT_JPEG]);
    frame.extend_from_slice(&(HEADER_LEN as u16).to_le_bytes());
    frame.extend_from_slice(&sequence.to_le_bytes());
    frame.extend_from_slice(&width.to_le_bytes());
    frame.extend_from_slice(&height.to_le_bytes());
    frame.extend_from_slice(&(jpeg.len() as u32).to_le_bytes());
    frame.extend_from_slice(&crc32fast::hash(jpeg).to_le_bytes());
    frame.extend_from_slice(jpeg);
    Ok(frame)
}

pub fn encode_udp_fragments(
    frame: &[u8],
    session_id: u32,
    session_token: u32,
    sequence: u32,
    max_payload: usize,
) -> Result<Vec<Vec<u8>>, String> {
    if frame.len() < HEADER_LEN || max_payload == 0 || max_payload > 1200 {
        return Err("invalid UDP frame or payload limit".to_string());
    }
    let fragment_count = frame.len().div_ceil(max_payload);
    if fragment_count > MAX_UDP_FRAGMENTS {
        return Err("UDP frame requires too many fragments".to_string());
    }
    let mut fragments = Vec::with_capacity(fragment_count);
    for (index, payload) in frame.chunks(max_payload).enumerate() {
        let offset = index * max_payload;
        let mut datagram = Vec::with_capacity(UDP_HEADER_LEN + payload.len());
        datagram.extend_from_slice(&UDP_MAGIC.to_le_bytes());
        datagram.extend_from_slice(&[VERSION, 1]);
        datagram.extend_from_slice(&(UDP_HEADER_LEN as u16).to_le_bytes());
        datagram.extend_from_slice(&session_id.to_le_bytes());
        datagram.extend_from_slice(&session_token.to_le_bytes());
        datagram.extend_from_slice(&sequence.to_le_bytes());
        datagram.extend_from_slice(&(frame.len() as u32).to_le_bytes());
        datagram.extend_from_slice(&(index as u16).to_le_bytes());
        datagram.extend_from_slice(&(fragment_count as u16).to_le_bytes());
        datagram.extend_from_slice(&(offset as u32).to_le_bytes());
        datagram.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        datagram.extend_from_slice(&0_u16.to_le_bytes());
        datagram.extend_from_slice(payload);
        fragments.push(datagram);
    }
    Ok(fragments)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn header_is_wire_compatible() {
        let frame = encode_frame(7, 368, 448, 100 * 1024, &[1, 2, 3]).unwrap();
        assert_eq!(frame.len(), HEADER_LEN + 3);
        assert_eq!(&frame[..4], b"CDP1");
        assert_eq!(
            u16::from_le_bytes(frame[6..8].try_into().unwrap()),
            HEADER_LEN as u16
        );
        assert_eq!(
            u32::from_le_bytes(frame[20..24].try_into().unwrap()),
            0x55_bc_80_1d
        );
    }

    #[test]
    fn rejects_oversized_jpeg() {
        assert!(encode_frame(1, 368, 448, 100, &[0; 101]).is_err());
    }

    #[test]
    fn udp_fragments_cover_the_complete_frame() {
        let frame = encode_frame(9, 1024, 600, 3000, &vec![7; 2500]).unwrap();
        let fragments = encode_udp_fragments(&frame, 3, 4, 9, 1200).unwrap();
        assert_eq!(fragments.len(), 3);
        assert_eq!(
            u32::from_le_bytes(fragments[0][..4].try_into().unwrap()),
            UDP_MAGIC
        );
        assert_eq!(
            u32::from_le_bytes(fragments[2][28..32].try_into().unwrap()),
            2400
        );
    }

    #[test]
    fn udp_rejects_invalid_payload_limits() {
        let frame = encode_frame(1, 368, 448, 100, &[9; 16]).unwrap();
        assert!(encode_udp_fragments(&frame, 1, 2, 1, 0).is_err());
        assert!(encode_udp_fragments(&frame, 1, 2, 1, 1201).is_err());
    }

    #[test]
    fn udp_rejects_too_many_fragments() {
        let frame = encode_frame(1, 368, 448, 1024, &vec![1; 1024]).unwrap();
        assert!(encode_udp_fragments(&frame, 1, 2, 1, 1).is_err());
    }

    #[test]
    fn udp_allows_the_protocol_fragment_ceiling() {
        let frame = encode_frame(42, 368, 448, 100 * 1024, &vec![5; 100 * 1024]).unwrap();
        let fragments = encode_udp_fragments(&frame, 7, 8, 42, 801).unwrap();
        assert_eq!(fragments.len(), 128);
        assert_eq!(
            u16::from_le_bytes(fragments[127][24..26].try_into().unwrap()),
            127
        );
        assert_eq!(
            u16::from_le_bytes(fragments[127][26..28].try_into().unwrap()),
            128
        );
    }

    #[test]
    fn udp_fragments_reconstruct_the_original_frame() {
        let frame = encode_frame(11, 1024, 600, 4096, &vec![3; 4096]).unwrap();
        let fragments = encode_udp_fragments(&frame, 9, 10, 11, 777).unwrap();
        let mut reconstructed = Vec::new();
        for fragment in fragments {
            reconstructed.extend_from_slice(&fragment[UDP_HEADER_LEN..]);
        }
        assert_eq!(reconstructed, frame);
    }

    #[test]
    fn frame_header_uses_negotiated_dimensions() {
        let frame = encode_frame(3, 1024, 600, 16, &[1, 2]).unwrap();
        assert_eq!(u16::from_le_bytes(frame[12..14].try_into().unwrap()), 1024);
        assert_eq!(u16::from_le_bytes(frame[14..16].try_into().unwrap()), 600);
    }
}
