// Prevents an extra console window on Windows in release. No-op elsewhere.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    maplecast_desktop_lib::run()
}
