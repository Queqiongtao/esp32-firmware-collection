use serde::Deserialize;

/* Gesture tuning in device panel pixels / device milliseconds. The engine only
uses differences of the device timestamps, so clock bases never matter. */
pub const LONG_PRESS_MS: u32 = 500;
pub const MOVE_THRESHOLD_PX: i32 = 8;
pub const SCROLL_EDGE_PX: u16 = 28;
pub const SCROLL_STEP_PX: i32 = 24;

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
    state: GestureState,
}

fn moved_beyond_threshold(x0: u16, y0: u16, x: u16, y: u16) -> bool {
    let dx = (i32::from(x) - i32::from(x0)).abs();
    let dy = (i32::from(y) - i32::from(y0)).abs();
    dx > MOVE_THRESHOLD_PX || dy > MOVE_THRESHOLD_PX
}

impl GestureEngine {
    pub fn new(panel_width: u16) -> Self {
        Self {
            panel_width,
            state: GestureState::Idle,
        }
    }

    fn in_scroll_zone(&self, x: u16) -> bool {
        x >= self.panel_width.saturating_sub(SCROLL_EDGE_PX)
    }

    fn drain_scroll(x0: u16, y: u16, accum: &mut i32, actions: &mut Vec<GestureAction>) {
        let steps = *accum / SCROLL_STEP_PX;
        if steps != 0 {
            *accum -= steps * SCROLL_STEP_PX;
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
                let moved = moved_beyond_threshold(x0, y0, event.x, event.y);
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
                        Self::drain_scroll(x0, event.y, &mut accum, &mut actions);
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
                Self::drain_scroll(x0, event.y, &mut accum, &mut actions);
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
        let mut engine = GestureEngine::new(PANEL_WIDTH);
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
        let mut engine = GestureEngine::new(PANEL_WIDTH);
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
        let mut engine = GestureEngine::new(PANEL_WIDTH);
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
        let mut engine = GestureEngine::new(PANEL_WIDTH);
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
        let mut engine = GestureEngine::new(PANEL_WIDTH);
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
        let mut engine = GestureEngine::new(PANEL_WIDTH);
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
        let mut engine = GestureEngine::new(PANEL_WIDTH);
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
}
