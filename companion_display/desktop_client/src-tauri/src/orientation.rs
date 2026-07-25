use image::{imageops, RgbImage};
use serde::Deserialize;

#[derive(Clone, Copy, Debug, Default, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum DisplayOrientation {
    #[default]
    Portrait,
    LandscapeLeft,
    PortraitInverted,
    LandscapeRight,
}

impl DisplayOrientation {
    pub fn logical_size(self, physical_width: u32, physical_height: u32) -> (u32, u32) {
        match self {
            Self::Portrait | Self::PortraitInverted => (physical_width, physical_height),
            Self::LandscapeLeft | Self::LandscapeRight => (physical_height, physical_width),
        }
    }

    pub fn map_logical_to_physical(
        self,
        x: u32,
        y: u32,
        physical_width: u32,
        physical_height: u32,
    ) -> (u32, u32) {
        match self {
            Self::Portrait => (x, y),
            Self::LandscapeLeft => (physical_width - 1 - y, x),
            Self::PortraitInverted => (physical_width - 1 - x, physical_height - 1 - y),
            Self::LandscapeRight => (y, physical_height - 1 - x),
        }
    }

    pub fn orient_logical_image(self, image: &RgbImage) -> RgbImage {
        match self {
            Self::Portrait => image.clone(),
            Self::LandscapeLeft => imageops::rotate90(image),
            Self::PortraitInverted => imageops::rotate180(image),
            Self::LandscapeRight => imageops::rotate270(image),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageBuffer, Rgb};

    #[test]
    fn logical_sizes_follow_physical_orientation() {
        assert_eq!(
            DisplayOrientation::Portrait.logical_size(368, 448),
            (368, 448)
        );
        assert_eq!(
            DisplayOrientation::LandscapeLeft.logical_size(368, 448),
            (448, 368)
        );
        assert_eq!(
            DisplayOrientation::PortraitInverted.logical_size(368, 448),
            (368, 448)
        );
        assert_eq!(
            DisplayOrientation::LandscapeRight.logical_size(368, 448),
            (448, 368)
        );
    }

    #[test]
    fn corner_mapping_matches_device_placement() {
        assert_eq!(
            DisplayOrientation::Portrait.map_logical_to_physical(0, 0, 3, 4),
            (0, 0)
        );
        assert_eq!(
            DisplayOrientation::LandscapeLeft.map_logical_to_physical(0, 0, 3, 4),
            (2, 0)
        );
        assert_eq!(
            DisplayOrientation::PortraitInverted.map_logical_to_physical(0, 0, 3, 4),
            (2, 3)
        );
        assert_eq!(
            DisplayOrientation::LandscapeRight.map_logical_to_physical(0, 0, 3, 4),
            (0, 3)
        );
    }

    #[test]
    fn image_rotation_keeps_native_device_dimensions() {
        let landscape = ImageBuffer::from_fn(4, 3, |x, y| Rgb([(y * 4 + x) as u8, 0, 0]));
        let left = DisplayOrientation::LandscapeLeft.orient_logical_image(&landscape);
        let right = DisplayOrientation::LandscapeRight.orient_logical_image(&landscape);
        assert_eq!(left.dimensions(), (3, 4));
        assert_eq!(right.dimensions(), (3, 4));
        assert_eq!(left.get_pixel(2, 0)[0], 0);
        assert_eq!(right.get_pixel(0, 3)[0], 0);
    }
}
