#[tauri::command]
fn get_quota_snapshot() -> quota_core::QuotaSnapshot {
    quota_core::get_quota_snapshot()
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![get_quota_snapshot])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
