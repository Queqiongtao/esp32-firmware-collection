use serde::Deserialize;

/* S3 reference tuning. Other panel sizes scale these distances while time
thresholds remain in device milliseconds. */
pub const LONG_PRESS_MS: u32 = 500;
const REFERENCE_SHORT_EDGE: u32 = 368;
const REFERENCE_HEIGHT: u32 = 448;
const REFERENCE_MOVE_THRESHOLD: u32 = 8;
const REFERENCE_SCROLL_EDGE: u32 = 28;
const REFERENCE_SCROLL_STEP: u32 = 24;

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
pub struct TouchDescriptor {
    pub width: u16,
    pub height: u16,
    pub max_move_hz: u16,
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum TouchEventKind {
    Down,
    Move,
    Up,
}

#[derive(Clone, Copy, Debug, Deserialize)]
pub struct TouchEvent {
    pub kind: TouchEventKind,
    pub x: u16,
    pub y: u16,
    pub t: u32,
}

fn normalize_axis(value: u16, source_size: u16, target_size: u16) -> u16 {
    if source_size <= 1 || target_size <= 1 {
        return 0;
    }
    let source_max = u32::from(source_size - 1);
    let target_max = u32::from(target_size - 1);
    ((u32::from(value).min(source_max) * target_max + source_max / 2) / source_max) as u16
}

impl TouchEvent {
    pub fn normalize(
        &self,
        descriptor: TouchDescriptor,
        target_width: u16,
        target_height: u16,
    ) -> Self {
        Self {
            x: normalize_axis(self.x, descriptor.width, target_width),
            y: normalize_axis(self.y, descriptor.height, target_height),
            ..*self
        }
    }
}

#[derive(Debug, Deserialize)]
pub struct TouchBatch {
    #[serde(rename = "type")]
    pub kind: String,
    pub seq: u32,
    pub events: Vec<TouchEvent>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MouseButton {
    Left,
    Right,
}

/* Actions carry device panel coordinates; the injection worker maps them to
source/screen coordinates before dispatching to a backend. */
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GestureAction {
    MoveTo { x: u16, y: u16 },
    ButtonDown { x: u16, y: u16, button: MouseButton },
    ButtonUp { button: MouseButton },
    Click { x: u16, y: u16, button: MouseButton },
    Scroll { x: u16, y: u16, steps: i32 },
}

enum GestureState {
    Idle,
    /* Finger down, gesture not yet disambiguated. */
    Pending { x0: u16, y0: u16, t0: u32 },
    Dragging,
    Scrolling { x0: u16, last_y: u16, accum: i32 },
    /* Long press already fired; ignore everything until the finger lifts. */
    Consumed,
}

pub struct GestureEngine {
    panel_width: u16,
    move_threshold: i32,
    scroll_edge: u16,
    scroll_step: i32,
    state: GestureState,
}

fn scale_distance(size: u32, reference_distance: u32, reference_size: u32) -> u32 {
    ((size * reference_distance + reference_size / 2) / reference_size).max(1)
}

fn moved_beyond_threshold(x0: u16, y0: u16, x: u16, y: u16, threshold: i32) -> bool {
    let dx = (i32::from(x) - i32::from(x0)).abs();
    let dy = (i32::from(y) - i32::from(y0)).abs();
    dx > threshold || dy > threshold
}

impl GestureEngine {
    pub fn new(panel_width: u16, panel_height: u16) -> Self {
        Self {
            panel_width,
            move_threshold: scale_distance(
                u32::from(panel_width.min(panel_height)),
                REFERENCE_MOVE_THRESHOLD,
                REFERENCE_SHORT_EDGE,
            ) as i32,
            scroll_edge: scale_distance(
                u32::from(panel_width),
                REFERENCE_SCROLL_EDGE,
                REFERENCE_SHORT_EDGE,
            ) as u16,
            scroll_step: scale_distance(
                u32::from(panel_height),
                REFERENCE_SCROLL_STEP,
                REFERENCE_HEIGHT,
            ) as i32,
            state: GestureState::Idle,
        }
    }

    fn in_scroll_zone(&self, x: u16) -> bool {
        x >= self.panel_width.saturating_sub(self.scroll_edge)
    }

    fn drain_scroll(&self, x0: u16, y: u16, accum: &mut i32, actions: &mut Vec<GestureAction>) {
        let steps = *accum / self.scroll_step;
        if steps != 0 {
            *accum -= steps * self.scroll_step;
            actions.push(GestureAction::Scroll { x: x0, y, steps });
        }
    }

    pub fn handle(&mut self, event: &TouchEvent) -> Vec<GestureAction> {
        let mut actions = Vec::new();
        let state = std::mem::replace(&mut self.state, GestureState::Idle);
        self.state = match (state, event.kind) {
            (GestureState::Idle, TouchEventKind::Down) => GestureState::Pending {
                x0: event.x,
                y0: event.y,
                t0: event.t,
            },
            (GestureState::Pending { x0, y0, t0 }, TouchEventKind::Move) => {
                let moved = moved_beyond_threshold(
                    x0,
                    y0,
                    event.x,
                    event.y,
                    self.move_threshold,
                );
                if !moved && event.t.wrapping_sub(t0) >= LONG_PRESS_MS {
                    actions.push(GestureAction::Click {
                        x: x0,
                        y: y0,
                        button: MouseButton::Right,
                    });
                    GestureState::Consumed
                } else if moved {
                    if self.in_scroll_zone(x0) {
                        let mut accum = i32::from(event.y) - i32::from(y0);
                        self.drain_scroll(x0, event.y, &mut accum, &mut actions);
                        GestureState::Scrolling {
                            x0,
                            last_y: event.y,
                            accum,
                        }
                    } else {
                        actions.push(GestureAction::ButtonDown {
                            x: x0,
                            y: y0,
                            button: MouseButton::Left,
                        });
                        actions.push(GestureAction::MoveTo {
                            x: event.x,
                            y: event.y,
                        });
                        GestureState::Dragging
                    }
                } else {
                    GestureState::Pending { x0, y0, t0 }
                }
            }
            (GestureState::Pending { x0, y0, t0 }, TouchEventKind::Up) => {
                let button = if event.t.wrapping_sub(t0) >= LONG_PRESS_MS {
                    MouseButton::Right
                } else {
                    MouseButton::Left
                };
                actions.push(GestureAction::Click { x: x0, y: y0, button });
                GestureState::Idle
            }
            (GestureState::Dragging, TouchEventKind::Move) => {
                actions.push(GestureAction::MoveTo {
                    x: event.x,
                    y: event.y,
                });
                GestureState::Dragging
            }
            (GestureState::Dragging, TouchEventKind::Up) => {
                /* Release at the last confirmed position: the driver's release
                coordinates may carry one final noise jump. */
                actions.push(GestureAction::ButtonUp {
                    button: MouseButton::Left,
                });
                GestureState::Idle
            }
            (
                GestureState::Scrolling {
                    x0,
                    last_y,
                    mut accum,
                },
                TouchEventKind::Move,
            ) => {
                accum += i32::from(event.y) - i32::from(last_y);
                self.drain_scroll(x0, event.y, &mut accum, &mut actions);
                GestureState::Scrolling {
                    x0,
                    last_y: event.y,
                    accum,
                }
            }
            (GestureState::Scrolling { .. } | GestureState::Consumed, TouchEventKind::Up) => {
                GestureState::Idle
            }
            /* A down while already tracking means the release was lost; end a
            held drag before restarting so the host button never latches. */
            (previous, TouchEventKind::Down) => {
                if matches!(previous, GestureState::Dragging) {
                    actions.push(GestureAction::ButtonUp {
                        button: MouseButton::Left,
                    });
                }
                GestureState::Pending {
                    x0: event.x,
                    y0: event.y,
                    t0: event.t,
                }
            }
            (previous, _) => previous,
        };
        actions
    }

    /* Fire time-based gestures (long press) while the finger rests without
    producing events. now_ms is an estimate on the device clock base. */
    pub fn tick(&mut self, now_ms: u32) -> Vec<GestureAction> {
        let mut actions = Vec::new();
        if let GestureState::Pending { x0, y0, t0 } = self.state {
            if now_ms.wrapping_sub(t0) >= LONG_PRESS_MS {
                actions.push(GestureAction::Click {
                    x: x0,
                    y: y0,
                    button: MouseButton::Right,
                });
                self.state = GestureState::Consumed;
            }
        }
        actions
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PANEL_WIDTH: u16 = 368;
    const PANEL_HEIGHT: u16 = 448;

    fn engine() -> GestureEngine {
        GestureEngine::new(PANEL_WIDTH, PANEL_HEIGHT)
    }

    fn event(kind: TouchEventKind, x: u16, y: u16, t: u32) -> TouchEvent {
        TouchEvent { kind, x, y, t }
    }

    #[test]
    fn touch_batch_parses_the_wire_format() {
        let json = r#"{"type":"touch_events","seq":7,"events":[
            {"kind":"down","x":184,"y":220,"t":1000},
            {"kind":"move","x":190,"y":224,"t":1016},
            {"kind":"up","x":190,"y":224,"t":1100}]}"#;
        let batch: TouchBatch = serde_json::from_str(json).unwrap();
        assert_eq!(batch.kind, "touch_events");
        assert_eq!(batch.seq, 7);
        assert_eq!(batch.events.len(), 3);
        assert_eq!(batch.events[0].kind, TouchEventKind::Down);
        assert_eq!(batch.events[2].kind, TouchEventKind::Up);
        assert_eq!(batch.events[1].x, 190);
    }

    #[test]
    fn quick_tap_clicks_left_at_the_down_position() {
        let mut engine = engine();
        assert!(engine
            .handle(&event(TouchEventKind::Down, 100, 120, 1000))
            .is_empty());
        let actions = engine.handle(&event(TouchEventKind::Up, 102, 121, 1120));
        assert_eq!(
            actions,
            vec![GestureAction::Click {
                x: 100,
                y: 120,
                button: MouseButton::Left
            }]
        );
    }

    #[test]
    fn movement_starts_a_left_drag_from_the_down_position() {
        let mut engine = engine();
        engine.handle(&event(TouchEventKind::Down, 100, 120, 1000));
        let actions = engine.handle(&event(TouchEventKind::Move, 120, 140, 1030));
        assert_eq!(
            actions,
            vec![
                GestureAction::ButtonDown {
                    x: 100,
                    y: 120,
                    button: MouseButton::Left
                },
                GestureAction::MoveTo { x: 120, y: 140 },
            ]
        );
        assert_eq!(
            engine.handle(&event(TouchEventKind::Move, 130, 150, 1050)),
            vec![GestureAction::MoveTo { x: 130, y: 150 }]
        );
        assert_eq!(
            engine.handle(&event(TouchEventKind::Up, 400, 150, 1100)),
            vec![GestureAction::ButtonUp {
                button: MouseButton::Left
            }]
        );
    }

    #[test]
    fn long_press_fires_a_right_click_from_tick() {
        let mut engine = engine();
        engine.handle(&event(TouchEventKind::Down, 50, 60, 1000));
        assert!(engine.tick(1400).is_empty());
        assert_eq!(
            engine.tick(1501),
            vec![GestureAction::Click {
                x: 50,
                y: 60,
                button: MouseButton::Right
            }]
        );
        /* The eventual release is swallowed. */
        assert!(engine
            .handle(&event(TouchEventKind::Up, 50, 60, 1600))
            .is_empty());
    }

    #[test]
    fn slow_release_without_tick_still_right_clicks() {
        let mut engine = engine();
        engine.handle(&event(TouchEventKind::Down, 50, 60, 1000));
        assert_eq!(
            engine.handle(&event(TouchEventKind::Up, 51, 61, 1700)),
            vec![GestureAction::Click {
                x: 50,
                y: 60,
                button: MouseButton::Right
            }]
        );
    }

    #[test]
    fn edge_swipe_scrolls_instead_of_dragging() {
        let mut engine = engine();
        let edge_x = PANEL_WIDTH - 10;
        engine.handle(&event(TouchEventKind::Down, edge_x, 200, 1000));
        /* 30 px downward: one step, 6 px remainder. */
        let actions = engine.handle(&event(TouchEventKind::Move, edge_x, 230, 1030));
        assert_eq!(
            actions,
            vec![GestureAction::Scroll {
                x: edge_x,
                y: 230,
                steps: 1
            }]
        );
        /* 18 px more accumulates to 24: another step. */
        let actions = engine.handle(&event(TouchEventKind::Move, edge_x, 248, 1060));
        assert_eq!(
            actions,
            vec![GestureAction::Scroll {
                x: edge_x,
                y: 248,
                steps: 1
            }]
        );
        assert!(engine
            .handle(&event(TouchEventKind::Up, edge_x, 248, 1100))
            .is_empty());
    }

    #[test]
    fn edge_swipe_up_scrolls_the_other_way() {
        let mut engine = engine();
        let edge_x = PANEL_WIDTH - 1;
        engine.handle(&event(TouchEventKind::Down, edge_x, 300, 1000));
        let actions = engine.handle(&event(TouchEventKind::Move, edge_x, 250, 1030));
        assert_eq!(
            actions,
            vec![GestureAction::Scroll {
                x: edge_x,
                y: 250,
                steps: -2
            }]
        );
    }

    #[test]
    fn stray_down_restarts_tracking() {
        let mut engine = engine();
        engine.handle(&event(TouchEventKind::Down, 10, 10, 1000));
        engine.handle(&event(TouchEventKind::Move, 40, 40, 1020));
        /* Missed release: a new down must first end the held drag. */
        assert_eq!(
            engine.handle(&event(TouchEventKind::Down, 200, 200, 2000)),
            vec![GestureAction::ButtonUp {
                button: MouseButton::Left
            }]
        );
        assert_eq!(
            engine.handle(&event(TouchEventKind::Up, 200, 200, 2100)),
            vec![GestureAction::Click {
                x: 200,
                y: 200,
                button: MouseButton::Left
            }]
        );
    }

    #[test]
    fn touch_coordinates_normalize_to_video_space() {
        let descriptor = TouchDescriptor {
            width: 800,
            height: 480,
            max_move_hz: 60,
        };
        let corner = event(TouchEventKind::Move, 799, 479, 10)
            .normalize(descriptor, 1024, 600);
        assert_eq!((corner.x, corner.y), (1023, 599));

        let center = event(TouchEventKind::Move, 400, 240, 10)
            .normalize(descriptor, 1024, 600);
        assert!((511..=513).contains(&center.x));
        assert!((299..=301).contains(&center.y));
    }

    #[test]
    fn p4_gesture_distances_scale_from_the_s3_reference() {
        let engine = GestureEngine::new(1024, 600);
        assert_eq!(engine.move_threshold, 13);
        assert_eq!(engine.scroll_edge, 78);
        assert_eq!(engine.scroll_step, 32);
    }
}
