mod protocol;

use tauri::Manager;

#[tauri::command]
fn protocol_preview(sequence: u32, jpeg_length: usize) -> Vec<u8> {
    protocol::encode_frame(sequence, &vec![0; jpeg_length])
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![protocol_preview])
        .setup(|app| {
            let _ = app.handle();
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("failed to run Companion Display");
}
