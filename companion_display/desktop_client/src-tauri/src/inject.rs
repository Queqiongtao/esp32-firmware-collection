use std::sync::{Arc, Mutex};

use crate::touch::{GestureAction, MouseButton};
use crate::touch_mapping::{map_panel_to_source, MappingParams};

/* Geometry shared between the stream thread (which learns source sizes and
region changes) and the touch worker (which maps panel points for injection).
Global coordinates are computed as (origin + source_point) * scale, letting
each backend pick its coordinate space: xcap monitors use their global origin
with scale 1, the Wayland portal uses stream-local coordinates with a
pixel-to-logical scale. */
#[derive(Clone, Copy, Debug)]
pub struct TouchGeometry {
    pub params: MappingParams,
    pub origin_x: f64,
    pub origin_y: f64,
    pub scale: f64,
}

pub type SharedTouchGeometry = Arc<Mutex<TouchGeometry>>;

pub trait InputInjector: Send {
    fn backend_name(&self) -> &'static str;
    fn pointer_move(&mut self, x: f64, y: f64) -> Result<(), String>;
    fn button(&mut self, button: MouseButton, down: bool) -> Result<(), String>;
    fn wheel(&mut self, steps: i32) -> Result<(), String>;
}

/* Fallback backend: logs what would be injected. Used for the test source and
whenever no platform backend is available, so the video keeps running. */
pub struct LogInjector;

impl InputInjector for LogInjector {
    fn backend_name(&self) -> &'static str {
        "log"
    }

    fn pointer_move(&mut self, x: f64, y: f64) -> Result<(), String> {
        eprintln!("touch inject (log): move to ({x:.1}, {y:.1})");
        Ok(())
    }

    fn button(&mut self, button: MouseButton, down: bool) -> Result<(), String> {
        eprintln!(
            "touch inject (log): {:?} button {}",
            button,
            if down { "down" } else { "up" }
        );
        Ok(())
    }

    fn wheel(&mut self, steps: i32) -> Result<(), String> {
        eprintln!("touch inject (log): wheel {steps}");
        Ok(())
    }
}

/* Windows/macOS injection through enigo (SendInput / CGEvent). macOS needs
the Accessibility permission; construction or injection failures degrade to
the log backend so the video stream never stops. */
#[cfg(not(target_os = "linux"))]
pub fn create_enigo_injector() -> Box<dyn InputInjector> {
    match enigo::Enigo::new(&enigo::Settings::default()) {
        Ok(enigo) => Box::new(EnigoInjector { enigo }),
        Err(error) => {
            eprintln!(
                "Touch control: system input backend unavailable ({error}); logging actions only"
            );
            Box::new(LogInjector)
        }
    }
}

#[cfg(not(target_os = "linux"))]
struct EnigoInjector {
    enigo: enigo::Enigo,
}

#[cfg(not(target_os = "linux"))]
impl InputInjector for EnigoInjector {
    fn backend_name(&self) -> &'static str {
        "enigo"
    }

    fn pointer_move(&mut self, x: f64, y: f64) -> Result<(), String> {
        use enigo::Mouse;
        self.enigo
            .move_mouse(x.round() as i32, y.round() as i32, enigo::Coordinate::Abs)
            .map_err(|error| error.to_string())
    }

    fn button(&mut self, button: MouseButton, down: bool) -> Result<(), String> {
        use enigo::Mouse;
        let button = match button {
            MouseButton::Left => enigo::Button::Left,
            MouseButton::Right => enigo::Button::Right,
        };
        let direction = if down {
            enigo::Direction::Press
        } else {
            enigo::Direction::Release
        };
        self.enigo
            .button(button, direction)
            .map_err(|error| error.to_string())
    }

    fn wheel(&mut self, steps: i32) -> Result<(), String> {
        use enigo::Mouse;
        /* Positive gesture steps mean the finger moved down; natural
        scrolling sends the wheel the other way. */
        self.enigo
            .scroll(-steps, enigo::Axis::Vertical)
            .map_err(|error| error.to_string())
    }
}

fn resolve(geometry: &TouchGeometry, x: u16, y: u16) -> Option<(f64, f64)> {
    let point = map_panel_to_source(&geometry.params, x, y)?;
    Some((
        (geometry.origin_x + point.x) * geometry.scale,
        (geometry.origin_y + point.y) * geometry.scale,
    ))
}

/* Executes gesture actions against the backend. Presses whose position lands
in the letterbox bars are skipped; a matching release still reaches the
backend, which treats a stray button-up as a no-op. */
pub fn dispatch_actions(
    actions: &[GestureAction],
    geometry: &SharedTouchGeometry,
    injector: &mut dyn InputInjector,
) {
    if actions.is_empty() {
        return;
    }
    let Ok(geometry) = geometry.lock().map(|guard| *guard) else {
        return;
    };
    for action in actions {
        let result = match *action {
            GestureAction::MoveTo { x, y } => match resolve(&geometry, x, y) {
                Some((gx, gy)) => injector.pointer_move(gx, gy),
                None => Ok(()),
            },
            GestureAction::ButtonDown { x, y, button } => match resolve(&geometry, x, y) {
                Some((gx, gy)) => injector
                    .pointer_move(gx, gy)
                    .and_then(|()| injector.button(button, true)),
                None => Ok(()),
            },
            GestureAction::ButtonUp { button } => injector.button(button, false),
            GestureAction::Click { x, y, button } => match resolve(&geometry, x, y) {
                Some((gx, gy)) => injector
                    .pointer_move(gx, gy)
                    .and_then(|()| injector.button(button, true))
                    .and_then(|()| injector.button(button, false)),
                None => Ok(()),
            },
            GestureAction::Scroll { x, y, steps } => match resolve(&geometry, x, y) {
                Some((gx, gy)) => injector
                    .pointer_move(gx, gy)
                    .and_then(|()| injector.wheel(steps)),
                None => Ok(()),
            },
        };
        if let Err(error) = result {
            eprintln!("touch inject ({}) failed: {error}", injector.backend_name());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::orientation::DisplayOrientation;

    #[derive(Default)]
    struct RecordingInjector {
        log: Vec<String>,
    }

    impl InputInjector for RecordingInjector {
        fn backend_name(&self) -> &'static str {
            "recording"
        }
        fn pointer_move(&mut self, x: f64, y: f64) -> Result<(), String> {
            self.log.push(format!("move {x:.0},{y:.0}"));
            Ok(())
        }
        fn button(&mut self, button: MouseButton, down: bool) -> Result<(), String> {
            self.log.push(format!("button {button:?} {down}"));
            Ok(())
        }
        fn wheel(&mut self, steps: i32) -> Result<(), String> {
            self.log.push(format!("wheel {steps}"));
            Ok(())
        }
    }

    fn geometry() -> SharedTouchGeometry {
        Arc::new(Mutex::new(TouchGeometry {
            params: MappingParams {
                panel_width: 368,
                panel_height: 448,
                orientation: DisplayOrientation::Portrait,
                region_x: 0,
                region_y: 0,
                region_width: 0,
                region_height: 0,
                source_width: 368,
                source_height: 448,
            },
            origin_x: 100.0,
            origin_y: 200.0,
            scale: 1.0,
        }))
    }

    #[test]
    fn click_moves_then_presses_and_releases_at_the_global_point() {
        let geometry = geometry();
        let mut injector = RecordingInjector::default();
        dispatch_actions(
            &[GestureAction::Click {
                x: 10,
                y: 20,
                button: MouseButton::Left,
            }],
            &geometry,
            &mut injector,
        );
        assert_eq!(
            injector.log,
            vec![
                "move 110,220".to_string(),
                "button Left true".to_string(),
                "button Left false".to_string(),
            ]
        );
    }

    #[test]
    fn actions_in_the_letterbox_are_skipped_but_release_passes() {
        let geometry = geometry();
        if let Ok(mut guard) = geometry.lock() {
            guard.params.source_width = 1920;
            guard.params.source_height = 1080;
        }
        let mut injector = RecordingInjector::default();
        dispatch_actions(
            &[
                GestureAction::ButtonDown {
                    x: 184,
                    y: 2,
                    button: MouseButton::Left,
                },
                GestureAction::ButtonUp {
                    button: MouseButton::Left,
                },
            ],
            &geometry,
            &mut injector,
        );
        assert_eq!(injector.log, vec!["button Left false".to_string()]);
    }
}
