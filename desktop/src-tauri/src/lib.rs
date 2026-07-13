//! MapleCast desktop shell.
//!
//! A thin Tauri v2 window that loads the LIVE WebGPU client (default
//! https://nobd.net/webgpu-test.html) and adds the one thing a browser cannot:
//! a native controller -> UDP:7100 input path polled off the webview thread.
//!
//! Design decisions (see desktop/README.md for the full rationale):
//!  * We load the client REMOTELY, not from a bundled copy — the web/ tree
//!    contains ROM-derived sprite rips (copyrighted, gitignored); shipping them
//!    in a downloadable binary is a worse distribution problem than committing
//!    them. Remote-load also gives WebGPU its required secure context for free
//!    and makes asset updates instant.
//!  * Input is polled natively in Rust (see input.rs). The page is asked (via
//!    `?native=1`) to SUPPRESS its own browser gamepad->WebSocket sender so we
//!    don't double-send.
//!  * A `maplecast://` deep link launches / focuses the app straight into a
//!    match — the "click a link and play" flow, upgraded to the native client.

mod input;

use tauri::Manager;
use tauri_plugin_deep_link::DeepLinkExt;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let cfg = input::InputConfig::from_env();
    let boot_url = cfg.boot_url();

    tauri::Builder::default()
        // Single-instance FIRST: a second launch (clicking another match link)
        // forwards its args here instead of opening a duplicate window.
        .plugin(tauri_plugin_single_instance::init(|app, args, _cwd| {
            if let Some(u) = args.iter().find(|a| a.starts_with("maplecast://")) {
                navigate_to_match(app, u);
            } else if let Some(w) = app.get_webview_window("main") {
                let _ = w.set_focus();
            }
        }))
        .plugin(tauri_plugin_deep_link::init())
        .setup(move |app| {
            // 1) The latency win: native gamepad -> UDP:7100, off the webview thread.
            input::spawn_input_thread(cfg.clone());

            // 2) Load the live client — or jump straight into a match if we were
            //    cold-started from a maplecast:// deep link.
            let start = app
                .deep_link()
                .get_current()
                .ok()
                .flatten()
                .and_then(|urls| urls.into_iter().next())
                .map(|u| input::match_url_from_deep_link(u.as_str(), &cfg))
                .unwrap_or_else(|| boot_url.clone());
            if let Some(w) = app.get_webview_window("main") {
                if let Ok(u) = start.parse() {
                    let _ = w.navigate(u);
                }
            }

            // 3) Warm deep links (app already running).
            let handle = app.handle().clone();
            app.deep_link().on_open_url(move |event| {
                if let Some(u) = event.urls().into_iter().next() {
                    navigate_to_match(&handle, u.as_str());
                }
            });
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running the MapleCast desktop shell");
}

/// Point the window at the match a `maplecast://` link encodes, and focus it.
fn navigate_to_match(app: &tauri::AppHandle, deep_url: &str) {
    let cfg = input::InputConfig::from_env();
    if let Some(w) = app.get_webview_window("main") {
        if let Ok(u) = input::match_url_from_deep_link(deep_url, &cfg).parse() {
            let _ = w.navigate(u);
        }
        let _ = w.set_focus();
    }
}
