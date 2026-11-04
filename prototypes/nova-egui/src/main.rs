#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod assets;
mod document;
mod io;
mod metrics;
mod pixels;
mod render;
mod ui;
use std::{path::PathBuf, time::Instant};
fn arg(args: &[String], key: &str) -> Option<String> {
    args.iter()
        .position(|a| a == key)
        .and_then(|i| args.get(i + 1))
        .cloned()
}
fn main() -> eframe::Result {
    let started = Instant::now();
    let args: Vec<String> = std::env::args().collect();
    if let Some(root) = arg(&args, "--bench") {
        let out = arg(&args, "--output").unwrap_or("rust-bench.json".into());
        if let Err(e) = metrics::bench(&PathBuf::from(root), &PathBuf::from(out)) {
            eprintln!("{e}");
            std::process::exit(1);
        }
        return Ok(());
    }
    let root =
        PathBuf::from(arg(&args, "--project").unwrap_or("fixtures/generated/pack-1500".into()));
    let smoke = arg(&args, "--smoke-seconds").and_then(|s| s.parse().ok());
    let telemetry = arg(&args, "--telemetry").map(PathBuf::from);
    let mut options = eframe::NativeOptions {
        renderer: eframe::Renderer::Wgpu,
        viewport: eframe::egui::ViewportBuilder::default()
            .with_inner_size([1280., 820.])
            .with_min_inner_size([800., 550.]),
        ..Default::default()
    };
    // Keep the portable wgpu renderer; use the measured low-idle Windows backend.
    // WGPU_BACKEND=vulkan remains an explicit diagnostic override.
    if cfg!(target_os = "windows")
        && std::env::var_os("WGPU_BACKEND").is_none()
        && let eframe::egui_wgpu::WgpuSetup::CreateNew(setup) = &mut options.wgpu_options.wgpu_setup
    {
        setup.instance_descriptor.backends = eframe::wgpu::Backends::DX12;
    }
    eframe::run_native(
        "Nova Studio · egui",
        options,
        Box::new(move |cc| Ok(Box::new(ui::Nova::new(cc, root, started, smoke, telemetry)))),
    )
}
