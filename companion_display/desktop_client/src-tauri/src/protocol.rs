pub const MAGIC: u32 = 0x3150_4443;
pub const VERSION: u8 = 1;
pub const FORMAT_JPEG: u8 = 1;
pub const HEADER_LEN: usize = 24;
pub const UDP_MAGIC: u32 = 0x3155_4443;
pub const UDP_HEADER_LEN: usize = 36;
pub const MAX_UDP_FRAGMENTS: usize = 128;
pub const UDP_FEC_FLAG: u16 = 0x8000;
pub const UDP_FEC_DATA_FRAGMENTS_PER_GROUP: usize = 8;

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
    fec: bool,
) -> Result<Vec<Vec<u8>>, String> {
    if frame.len() < HEADER_LEN || max_payload == 0 || max_payload > 1200 {
        return Err("invalid UDP frame or payload limit".to_string());
    }
    let fragment_count = frame.len().div_ceil(max_payload);
    if fragment_count > MAX_UDP_FRAGMENTS {
        return Err("UDP frame requires too many fragments".to_string());
    }
    let fec_groups = if fec {
        fragment_count.div_ceil(UDP_FEC_DATA_FRAGMENTS_PER_GROUP)
    } else {
        0
    };
    let mut fragments = Vec::with_capacity(fragment_count + fec_groups);
    let mut parity = vec![vec![0_u8; max_payload]; fec_groups];
    for (index, payload) in frame.chunks(max_payload).enumerate() {
        let offset = index * max_payload;
        if fec {
            let group = index % fec_groups;
            for (target, source) in parity[group].iter_mut().zip(payload) {
                *target ^= source;
            }
        }
        fragments.push(build_udp_datagram(
            frame.len(),
            session_id,
            session_token,
            sequence,
            index,
            fragment_count,
            offset,
            0,
            payload,
        ));
    }
    for (group, payload) in parity.iter().enumerate() {
        fragments.push(build_udp_datagram(
            frame.len(),
            session_id,
            session_token,
            sequence,
            group,
            fragment_count,
            0,
            UDP_FEC_FLAG | group as u16,
            payload,
        ));
    }
    Ok(fragments)
}

#[allow(clippy::too_many_arguments)]
fn build_udp_datagram(
    frame_length: usize,
    session_id: u32,
    session_token: u32,
    sequence: u32,
    fragment_index: usize,
    fragment_count: usize,
    fragment_offset: usize,
    reserved: u16,
    payload: &[u8],
) -> Vec<u8> {
    let mut datagram = Vec::with_capacity(UDP_HEADER_LEN + payload.len());
    datagram.extend_from_slice(&UDP_MAGIC.to_le_bytes());
    datagram.extend_from_slice(&[VERSION, 1]);
    datagram.extend_from_slice(&(UDP_HEADER_LEN as u16).to_le_bytes());
    datagram.extend_from_slice(&session_id.to_le_bytes());
    datagram.extend_from_slice(&session_token.to_le_bytes());
    datagram.extend_from_slice(&sequence.to_le_bytes());
    datagram.extend_from_slice(&(frame_length as u32).to_le_bytes());
    datagram.extend_from_slice(&(fragment_index as u16).to_le_bytes());
    datagram.extend_from_slice(&(fragment_count as u16).to_le_bytes());
    datagram.extend_from_slice(&(fragment_offset as u32).to_le_bytes());
    datagram.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    datagram.extend_from_slice(&reserved.to_le_bytes());
    datagram.extend_from_slice(payload);
    datagram
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
        let fragments = encode_udp_fragments(&frame, 3, 4, 9, 1200, false).unwrap();
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
        assert!(encode_udp_fragments(&frame, 1, 2, 1, 0, false).is_err());
        assert!(encode_udp_fragments(&frame, 1, 2, 1, 1201, false).is_err());
    }

    #[test]
    fn udp_rejects_too_many_fragments() {
        let frame = encode_frame(1, 368, 448, 1024, &vec![1; 1024]).unwrap();
        assert!(encode_udp_fragments(&frame, 1, 2, 1, 1, false).is_err());
    }

    #[test]
    fn udp_allows_the_protocol_fragment_ceiling() {
        let frame = encode_frame(42, 368, 448, 100 * 1024, &vec![5; 100 * 1024]).unwrap();
        let fragments = encode_udp_fragments(&frame, 7, 8, 42, 801, false).unwrap();
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
        let fragments = encode_udp_fragments(&frame, 9, 10, 11, 777, false).unwrap();
        let mut reconstructed = Vec::new();
        for fragment in fragments {
            reconstructed.extend_from_slice(&fragment[UDP_HEADER_LEN..]);
        }
        assert_eq!(reconstructed, frame);
    }

    #[test]
    fn udp_fec_recovers_one_missing_fragment_per_interleaved_group() {
        let frame = encode_frame(12, 1024, 600, 16_000, &vec![3; 15_000]).unwrap();
        let max_payload = 1200;
        let packets = encode_udp_fragments(&frame, 9, 10, 12, max_payload, true).unwrap();
        let data_count = frame.len().div_ceil(max_payload);
        let group_count = data_count.div_ceil(UDP_FEC_DATA_FRAGMENTS_PER_GROUP);
        assert_eq!(packets.len(), data_count + group_count);

        let mut recovered = vec![0_u8; frame.len()];
        let missing = [0_usize, 1_usize];
        for (index, packet) in packets[..data_count].iter().enumerate() {
            if missing.contains(&index) {
                continue;
            }
            let offset = index * max_payload;
            recovered[offset..offset + packet.len() - UDP_HEADER_LEN]
                .copy_from_slice(&packet[UDP_HEADER_LEN..]);
        }
        for group in 0..group_count {
            let mut value = packets[data_count + group][UDP_HEADER_LEN..].to_vec();
            let absent = missing
                .iter()
                .copied()
                .find(|index| index % group_count == group);
            let Some(absent) = absent else { continue };
            for index in (group..data_count).step_by(group_count) {
                if index == absent {
                    continue;
                }
                let offset = index * max_payload;
                let length = (frame.len() - offset).min(max_payload);
                for byte in 0..length {
                    value[byte] ^= recovered[offset + byte];
                }
            }
            let offset = absent * max_payload;
            let length = (frame.len() - offset).min(max_payload);
            recovered[offset..offset + length].copy_from_slice(&value[..length]);
        }
        assert_eq!(recovered, frame);
    }

    #[test]
    fn frame_header_uses_negotiated_dimensions() {
        let frame = encode_frame(3, 1024, 600, 16, &[1, 2]).unwrap();
        assert_eq!(u16::from_le_bytes(frame[12..14].try_into().unwrap()), 1024);
        assert_eq!(u16::from_le_bytes(frame[14..16].try_into().unwrap()), 600);
    }
}
