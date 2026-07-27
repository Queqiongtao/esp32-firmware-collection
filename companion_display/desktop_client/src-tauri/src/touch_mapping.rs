use crate::orientation::DisplayOrientation;

#[derive(Clone, Copy, Debug)]
pub struct MappingParams {
    pub panel_width: u32,
    pub panel_height: u32,
    pub orientation: DisplayOrientation,
    pub region_x: u32,
    pub region_y: u32,
    /* 0 means the full effective source, mirroring StreamConfig regions. */
    pub region_width: u32,
    pub region_height: u32,
    pub source_width: u32,
    pub source_height: u32,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SourcePoint {
    pub x: f64,
    pub y: f64,
}

impl MappingParams {
    fn crop(&self) -> (u32, u32, u32, u32) {
        let width = if self.region_width == 0 {
            self.source_width
        } else {
            self.region_width
        };
        let height = if self.region_height == 0 {
            self.source_height
        } else {
            self.region_height
        };
        (self.region_x, self.region_y, width, height)
    }
}

/* Inverse of the encode pipeline (crop -> scale+letterbox -> rotate): maps a
device panel coordinate back into source pixels. The letterbox math mirrors
wayland_capture::letterbox_layout and crop_and_letterbox. Returns None for
touches landing in the letterbox bars or before the source size is known. */
pub fn map_panel_to_source(
    params: &MappingParams,
    panel_x: u16,
    panel_y: u16,
) -> Option<SourcePoint> {
    if params.panel_width == 0
        || params.panel_height == 0
        || params.source_width == 0
        || params.source_height == 0
    {
        return None;
    }
    let (crop_x, crop_y, crop_width, crop_height) = params.crop();
    if crop_width == 0 || crop_height == 0 {
        return None;
    }
    let px = u32::from(panel_x).min(params.panel_width - 1);
    let py = u32::from(panel_y).min(params.panel_height - 1);
    let (logical_x, logical_y) =
        params
            .orientation
            .map_physical_to_logical(px, py, params.panel_width, params.panel_height);
    let (logical_width, logical_height) = params
        .orientation
        .logical_size(params.panel_width, params.panel_height);
    let scale =
        (logical_width as f32 / crop_width as f32).min(logical_height as f32 / crop_height as f32);
    let draw_width = ((crop_width as f32 * scale).round() as u32)
        .max(1)
        .min(logical_width);
    let draw_height = ((crop_height as f32 * scale).round() as u32)
        .max(1)
        .min(logical_height);
    let offset_x = (logical_width - draw_width) / 2;
    let offset_y = (logical_height - draw_height) / 2;
    if logical_x < offset_x || logical_y < offset_y {
        return None;
    }
    let content_x = logical_x - offset_x;
    let content_y = logical_y - offset_y;
    if content_x >= draw_width || content_y >= draw_height {
        return None;
    }
    Some(SourcePoint {
        x: f64::from(crop_x)
            + (f64::from(content_x) + 0.5) * f64::from(crop_width) / f64::from(draw_width),
        y: f64::from(crop_y)
            + (f64::from(content_y) + 0.5) * f64::from(crop_height) / f64::from(draw_height),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn params(source_width: u32, source_height: u32) -> MappingParams {
        MappingParams {
            panel_width: 368,
            panel_height: 448,
            orientation: DisplayOrientation::Portrait,
            region_x: 0,
            region_y: 0,
            region_width: 0,
            region_height: 0,
            source_width,
            source_height,
        }
    }

    #[test]
    fn matching_aspect_source_maps_one_to_one() {
        let params = params(368, 448);
        assert_eq!(
            map_panel_to_source(&params, 0, 0),
            Some(SourcePoint { x: 0.5, y: 0.5 })
        );
        assert_eq!(
            map_panel_to_source(&params, 367, 447),
            Some(SourcePoint { x: 367.5, y: 447.5 })
        );
    }

    #[test]
    fn wide_source_rejects_letterbox_bars_and_scales_content() {
        /* 1920x1080 into 368x448: draw 368x207, vertical bars of 120/121 px. */
        let params = params(1920, 1080);
        assert_eq!(map_panel_to_source(&params, 184, 0), None);
        assert_eq!(map_panel_to_source(&params, 184, 119), None);
        assert_eq!(map_panel_to_source(&params, 184, 340), None);
        let top = map_panel_to_source(&params, 0, 120).unwrap();
        assert!(top.x > 0.0 && top.x < 6.0, "top.x = {}", top.x);
        assert!(top.y > 0.0 && top.y < 6.0, "top.y = {}", top.y);
        let bottom = map_panel_to_source(&params, 367, 326).unwrap();
        assert!(
            bottom.x < 1920.0 && bottom.x > 1914.0,
            "bottom.x = {}",
            bottom.x
        );
        assert!(
            bottom.y < 1080.0 && bottom.y > 1074.0,
            "bottom.y = {}",
            bottom.y
        );
    }

    #[test]
    fn landscape_orientation_unrotates_before_mapping() {
        /* Landscape-left: logical canvas is 448x368; 1920x1080 scales to a
        448x252 draw area with 58 px letterbox bars. The inverse rotation is
        physical (px, py) -> logical (py, 367 - px), so logical (0, 58) - the
        first content pixel - sits at physical (309, 0). */
        let mut params = params(1920, 1080);
        params.orientation = DisplayOrientation::LandscapeLeft;
        assert!(map_panel_to_source(&params, 367, 0).is_none());
        assert!(map_panel_to_source(&params, 367 - 57, 0).is_none());
        let content = map_panel_to_source(&params, 367 - 58, 0).unwrap();
        assert!(content.x < 5.0, "content.x = {}", content.x);
        assert!(content.y < 5.0, "content.y = {}", content.y);
    }

    #[test]
    fn region_offsets_apply_to_the_mapped_point() {
        let mut params = params(1920, 1080);
        params.region_x = 100;
        params.region_y = 200;
        params.region_width = 368;
        params.region_height = 448;
        let point = map_panel_to_source(&params, 0, 0).unwrap();
        assert_eq!(point, SourcePoint { x: 100.5, y: 200.5 });
        let point = map_panel_to_source(&params, 367, 447).unwrap();
        assert_eq!(point, SourcePoint { x: 467.5, y: 647.5 });
    }

    #[test]
    fn unknown_source_size_maps_nothing() {
        let params = params(0, 0);
        assert_eq!(map_panel_to_source(&params, 10, 10), None);
    }
}
