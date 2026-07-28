use image::{DynamicImage, RgbImage};

const SHARPEN_SCALE_THRESHOLD_PERCENT: u16 = 90;
const SHARPEN_DETAIL_THRESHOLD: i32 = 3;
const SHARPEN_STRENGTH_PERCENT: i32 = 35;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

pub struct PreparedImage {
    pub image: DynamicImage,
    pub source_scale_percent: u16,
    pub content_rect: Option<ImageRect>,
    pub capture_process_ms: u32,
}

#[derive(Default)]
pub struct SharpenWorkspace {
    luma: Vec<u8>,
    horizontal: Vec<u16>,
}

impl PreparedImage {
    pub fn apply_quality_filter(&mut self, workspace: &mut SharpenWorkspace) {
        if !should_sharpen(self.source_scale_percent) {
            return;
        }
        let (Some(rect), Some(image)) = (self.content_rect, self.image.as_mut_rgb8()) else {
            return;
        };
        workspace.sharpen_luma_region(image, rect);
    }
}

pub fn scale_percent(
    source_width: u32,
    source_height: u32,
    draw_width: u32,
    draw_height: u32,
) -> u16 {
    if source_width == 0 || source_height == 0 {
        return 0;
    }
    let horizontal = u64::from(draw_width) * 100 / u64::from(source_width);
    let vertical = u64::from(draw_height) * 100 / u64::from(source_height);
    horizontal.min(vertical).min(u64::from(u16::MAX)) as u16
}

pub fn should_sharpen(scale_percent: u16) -> bool {
    scale_percent > 0 && scale_percent < SHARPEN_SCALE_THRESHOLD_PERCENT
}

#[cfg(test)]
pub fn sharpen_luma_region(image: &mut RgbImage, rect: ImageRect) {
    SharpenWorkspace::default().sharpen_luma_region(image, rect);
}

impl SharpenWorkspace {
    pub fn sharpen_luma_region(&mut self, image: &mut RgbImage, rect: ImageRect) {
        if rect.width < 3
            || rect.height < 3
            || rect.x > image.width().saturating_sub(rect.width)
            || rect.y > image.height().saturating_sub(rect.height)
        {
            return;
        }

        let image_width = image.width() as usize;
        let rect_x = rect.x as usize;
        let rect_y = rect.y as usize;
        let width = rect.width as usize;
        let height = rect.height as usize;
        self.luma.resize(width * height, 0);
        self.horizontal.resize(width * height, 0);
        let pixels = image.as_raw();
        for y in 0..height {
            let source_row = ((rect_y + y) * image_width + rect_x) * 3;
            let luma_row = y * width;
            for x in 0..width {
                let offset = source_row + x * 3;
                self.luma[luma_row + x] = ((77_u32 * u32::from(pixels[offset])
                    + 150_u32 * u32::from(pixels[offset + 1])
                    + 29_u32 * u32::from(pixels[offset + 2])
                    + 128)
                    >> 8) as u8;
            }
        }

        for y in 0..height {
            let row = y * width;
            for x in 1..width - 1 {
                self.horizontal[row + x] = u16::from(self.luma[row + x - 1])
                    + 2 * u16::from(self.luma[row + x])
                    + u16::from(self.luma[row + x + 1]);
            }
        }

        let pixels = image.as_mut();
        for y in 1..height - 1 {
            for x in 1..width - 1 {
                let offset = y * width + x;
                let softened = (i32::from(self.horizontal[offset - width])
                    + 2 * i32::from(self.horizontal[offset])
                    + i32::from(self.horizontal[offset + width])
                    + 8)
                    / 16;
                let detail = i32::from(self.luma[offset]) - softened;
                if detail.abs() < SHARPEN_DETAIL_THRESHOLD {
                    continue;
                }
                let weighted = detail * SHARPEN_STRENGTH_PERCENT;
                let adjustment = if weighted >= 0 {
                    (weighted + 50) / 100
                } else {
                    (weighted - 50) / 100
                };
                let pixel_offset = ((rect_y + y) * image_width + rect_x + x) * 3;
                for channel in &mut pixels[pixel_offset..pixel_offset + 3] {
                    *channel = (i32::from(*channel) + adjustment).clamp(0, 255) as u8;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageBuffer, Rgb};

    fn reference_sharpen(image: &mut RgbImage, rect: ImageRect) {
        let image_width = image.width() as usize;
        let rect_x = rect.x as usize;
        let rect_y = rect.y as usize;
        let width = rect.width as usize;
        let height = rect.height as usize;
        let pixels = image.as_raw();
        let mut luma = vec![0_u8; width * height];
        for y in 0..height {
            let source_row = ((rect_y + y) * image_width + rect_x) * 3;
            for x in 0..width {
                let offset = source_row + x * 3;
                luma[y * width + x] = ((77_u32 * u32::from(pixels[offset])
                    + 150_u32 * u32::from(pixels[offset + 1])
                    + 29_u32 * u32::from(pixels[offset + 2])
                    + 128)
                    >> 8) as u8;
            }
        }
        let pixels = image.as_mut();
        for y in 1..height - 1 {
            for x in 1..width - 1 {
                let offset = y * width + x;
                let softened = (i32::from(luma[offset - width - 1])
                    + 2 * i32::from(luma[offset - width])
                    + i32::from(luma[offset - width + 1])
                    + 2 * i32::from(luma[offset - 1])
                    + 4 * i32::from(luma[offset])
                    + 2 * i32::from(luma[offset + 1])
                    + i32::from(luma[offset + width - 1])
                    + 2 * i32::from(luma[offset + width])
                    + i32::from(luma[offset + width + 1])
                    + 8)
                    / 16;
                let detail = i32::from(luma[offset]) - softened;
                if detail.abs() < SHARPEN_DETAIL_THRESHOLD {
                    continue;
                }
                let weighted = detail * SHARPEN_STRENGTH_PERCENT;
                let adjustment = if weighted >= 0 {
                    (weighted + 50) / 100
                } else {
                    (weighted - 50) / 100
                };
                let pixel_offset = ((rect_y + y) * image_width + rect_x + x) * 3;
                for channel in &mut pixels[pixel_offset..pixel_offset + 3] {
                    *channel = (i32::from(*channel) + adjustment).clamp(0, 255) as u8;
                }
            }
        }
    }

    #[test]
    fn scale_percent_reports_downscale_and_upscale() {
        assert_eq!(scale_percent(1920, 1080, 1024, 576), 53);
        assert_eq!(scale_percent(800, 480, 1000, 600), 125);
        assert_eq!(scale_percent(0, 480, 1000, 600), 0);
    }

    #[test]
    fn sharpening_keeps_uniform_pixels_and_outside_region_unchanged() {
        let mut image = ImageBuffer::from_pixel(8, 6, Rgb([40, 80, 120]));
        for y in 1..5 {
            for x in 2..6 {
                image.put_pixel(
                    x,
                    y,
                    Rgb(if x < 4 { [20, 20, 20] } else { [220, 220, 220] }),
                );
            }
        }
        let outside = *image.get_pixel(0, 0);
        sharpen_luma_region(
            &mut image,
            ImageRect {
                x: 2,
                y: 1,
                width: 4,
                height: 4,
            },
        );
        assert_eq!(*image.get_pixel(0, 0), outside);
        assert_eq!(image.get_pixel(2, 2).0, [20, 20, 20]);
        assert!(image.get_pixel(3, 2).0[0] < 20);
        assert!(image.get_pixel(4, 2).0[0] > 220);
    }

    #[test]
    fn prepared_image_only_sharpens_downscaled_content() {
        let source = ImageBuffer::from_fn(8, 6, |x, _| {
            Rgb(if x < 4 { [20, 20, 20] } else { [220, 220, 220] })
        });
        let mut downscaled = PreparedImage {
            image: DynamicImage::ImageRgb8(source.clone()),
            source_scale_percent: 50,
            content_rect: Some(ImageRect {
                x: 1,
                y: 1,
                width: 6,
                height: 4,
            }),
            capture_process_ms: 0,
        };
        let mut workspace = SharpenWorkspace::default();
        downscaled.apply_quality_filter(&mut workspace);
        assert_eq!(downscaled.image.to_rgb8().get_pixel(0, 2).0, [20, 20, 20]);
        assert!(downscaled.image.to_rgb8().get_pixel(3, 2).0[0] < 20);

        let mut native = PreparedImage {
            image: DynamicImage::ImageRgb8(source.clone()),
            source_scale_percent: 100,
            content_rect: Some(ImageRect {
                x: 0,
                y: 0,
                width: 8,
                height: 6,
            }),
            capture_process_ms: 0,
        };
        native.apply_quality_filter(&mut workspace);
        assert_eq!(native.image.to_rgb8(), source);
    }

    #[test]
    fn reusable_workspace_matches_reference_across_sizes_and_rectangles() {
        let mut workspace = SharpenWorkspace::default();
        for (width, height, rect) in [
            (
                11,
                9,
                ImageRect {
                    x: 1,
                    y: 2,
                    width: 8,
                    height: 6,
                },
            ),
            (
                7,
                5,
                ImageRect {
                    x: 0,
                    y: 0,
                    width: 7,
                    height: 5,
                },
            ),
        ] {
            let source = ImageBuffer::from_fn(width, height, |x, y| {
                Rgb([
                    ((x * 37 + y * 11) & 0xff) as u8,
                    ((x * 13 + y * 41) & 0xff) as u8,
                    ((x * 23 + y * 17) & 0xff) as u8,
                ])
            });
            let mut expected = source.clone();
            let mut actual = source;
            reference_sharpen(&mut expected, rect);
            workspace.sharpen_luma_region(&mut actual, rect);
            assert_eq!(actual, expected);
        }
    }
}
