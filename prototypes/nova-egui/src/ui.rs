use crate::{
    assets::{Index, Navigation},
    document::Document,
    metrics::Metrics,
    render,
};
use eframe::egui::{self, Color32, Pos2, Rect, TextureHandle, TextureOptions, Vec2};
use egui_dock::widgets::tab_viewer::OnCloseResponse;
use egui_dock::{DockArea, DockState, NodeIndex, TabViewer};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque},
    path::{Path, PathBuf},
    sync::mpsc,
    time::{Duration, Instant},
};

#[cfg(test)]
mod layout_tests {
    use super::*;
    #[test]
    fn rearranged_workspace_roundtrip() {
        let mut dock = default_layout();
        dock.add_window(vec![Tab::Welcome]);
        dock.push_to_focused_leaf(Tab::Editor(PathBuf::from("a.png")));
        dock.push_to_focused_leaf(Tab::Editor(PathBuf::from("b.png")));
        let workspace = Workspace {
            dock,
            nav: Navigation {
                directory: "assets/a".into(),
                selected: Some("assets/a/a.png".into()),
                query: "stone".into(),
                grid: true,
                reverse: true,
            },
            dark: false,
            thumb_size: 96.,
            active: Some(PathBuf::from("a.png")),
        };
        let data =
            ron::ser::to_string_pretty(&workspace, ron::ser::PrettyConfig::default()).unwrap();
        let restored: Workspace = ron::from_str(&data).unwrap();
        assert_eq!(
            ron::ser::to_string_pretty(&restored, ron::ser::PrettyConfig::default()).unwrap(),
            data
        );
        assert_eq!(
            restored
                .dock
                .iter_all_tabs()
                .filter(|(_, t)| matches!(t, Tab::Editor(_)))
                .count(),
            2
        );
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Tab {
    Assets,
    Editor(PathBuf),
    Preview,
    Inspector,
    Welcome,
}
#[derive(Serialize, Deserialize)]
struct Workspace {
    dock: DockState<Tab>,
    nav: Navigation,
    dark: bool,
    thumb_size: f32,
    #[serde(default)]
    active: Option<PathBuf>,
}
struct Editor {
    doc: Document,
    texture: Option<TextureHandle>,
    zoom: f32,
    pan: Vec2,
    last: Option<[u32; 2]>,
    drawing: bool,
}
impl Editor {
    fn sync(&mut self, ctx: &egui::Context, metrics: &mut Metrics) {
        if self.texture.is_none() {
            let im = egui::ColorImage::from_rgba_unmultiplied(
                [
                    self.doc.image.width() as usize,
                    self.doc.image.height() as usize,
                ],
                self.doc.image.as_raw(),
            );
            self.texture = Some(ctx.load_texture(
                self.doc.path.to_string_lossy(),
                im,
                TextureOptions::NEAREST,
            ));
            metrics.count("upload_bytes", self.doc.image.as_raw().len() as u64);
            metrics.count("full_uploads", 1);
            self.doc.dirty = None;
        }
        if let Some([x, y, r, b]) = self.doc.dirty.take() {
            let im = image::imageops::crop_imm(&self.doc.image, x, y, r - x, b - y).to_image();
            let im = egui::ColorImage::from_rgba_unmultiplied(
                [(r - x) as usize, (b - y) as usize],
                im.as_raw(),
            );
            self.texture.as_mut().unwrap().set_partial(
                [x as usize, y as usize],
                im,
                TextureOptions::NEAREST,
            );
            metrics.count("partial_uploads", 1);
            metrics.count("upload_bytes", ((r - x) * (b - y) * 4) as u64);
        }
    }
}
type IndexReply = Result<(Index, f64), String>;
type ThumbReply = (PathBuf, Result<image::RgbaImage, String>);
struct Core {
    root: PathBuf,
    path_text: String,
    index: Index,
    nav: Navigation,
    rows: Vec<(PathBuf, usize, bool)>,
    expanded: BTreeSet<PathBuf>,
    refresh_rows: bool,
    index_rx: Option<mpsc::Receiver<IndexReply>>,
    thumb_tx: mpsc::SyncSender<PathBuf>,
    thumb_rx: mpsc::Receiver<ThumbReply>,
    pending: HashSet<PathBuf>,
    thumbs: HashMap<PathBuf, TextureHandle>,
    lru: VecDeque<PathBuf>,
    failed: HashSet<PathBuf>,
    editors: BTreeMap<PathBuf, Editor>,
    active: Option<PathBuf>,
    open_request: Option<PathBuf>,
    color: [u8; 4],
    eraser: bool,
    grid: bool,
    thumb_size: f32,
    dark: bool,
    status: String,
    metrics: Metrics,
    angles: [f32; 2],
    cube_scale: f32,
}
fn default_layout() -> DockState<Tab> {
    let mut dock = DockState::new(vec![Tab::Welcome]);
    let [center, _] =
        dock.main_surface_mut()
            .split_left(NodeIndex::root(), 0.23, vec![Tab::Assets]);
    let [center, _] = dock
        .main_surface_mut()
        .split_right(center, 0.77, vec![Tab::Inspector]);
    dock.main_surface_mut()
        .split_below(center, 0.68, vec![Tab::Preview]);
    dock
}
pub struct Nova {
    core: Core,
    dock: DockState<Tab>,
    started: Instant,
    telemetry: Option<PathBuf>,
    first: bool,
    ready_requested: bool,
    close_prompt: bool,
    qa: Option<PathBuf>,
    qa_step: usize,
    qa_next: Instant,
    exercise: bool,
    last_frame: Instant,
}
impl Nova {
    pub fn new(
        cc: &eframe::CreationContext<'_>,
        root: PathBuf,
        started: Instant,
        smoke: Option<f64>,
        telemetry: Option<PathBuf>,
    ) -> Self {
        let (tx, rx) = mpsc::sync_channel::<PathBuf>(256);
        // A smoke-test deadline must not keep the native event loop polling.
        if let Some(seconds) = smoke {
            let ctx = cc.egui_ctx.clone();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_secs_f64(seconds.max(0.01)));
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                ctx.request_repaint();
            });
        }
        let (done_tx, done_rx) = mpsc::sync_channel(256);
        let ctx = cc.egui_ctx.clone();
        std::thread::spawn(move || {
            while let Ok(path) = rx.recv() {
                let result = crate::io::load(&path).map(|im| {
                    image::imageops::resize(&im, 64, 64, image::imageops::FilterType::Nearest)
                });
                if done_tx.send((path, result)).is_err() {
                    break;
                }
                ctx.request_repaint();
            }
        });
        let saved = if std::env::args().any(|s| s == "--fresh-layout") {
            None
        } else {
            std::fs::read(root.join(".nova/egui-layout.ron"))
                .ok()
                .and_then(|v| ron::de::from_bytes::<Workspace>(&v).ok())
        };
        let layout_restored = saved.is_some();
        let saved_active = saved.as_ref().and_then(|w| w.active.clone());
        let (mut dock, nav, dark, thumb_size) = saved
            .map(|w| (w.dock, w.nav, w.dark, w.thumb_size))
            .unwrap_or((default_layout(), Navigation::default(), true, 72.0));
        let mut core = Core {
            root: root.clone(),
            path_text: root.to_string_lossy().into(),
            index: Index::default(),
            nav,
            rows: vec![],
            expanded: BTreeSet::new(),
            refresh_rows: true,
            index_rx: None,
            thumb_tx: tx,
            thumb_rx: done_rx,
            pending: HashSet::new(),
            thumbs: HashMap::new(),
            lru: VecDeque::new(),
            failed: HashSet::new(),
            editors: BTreeMap::new(),
            active: None,
            open_request: None,
            color: [130, 141, 255, 255],
            eraser: false,
            grid: true,
            thumb_size,
            dark,
            status: "Ready".into(),
            metrics: Metrics::default(),
            angles: [-0.45, 0.65],
            cube_scale: 1.0,
        };
        let paths: Vec<_> = dock
            .iter_all_tabs()
            .filter_map(|(_, tab)| {
                if let Tab::Editor(p) = tab {
                    Some(p.clone())
                } else {
                    None
                }
            })
            .collect();
        core.metrics
            .count("layout_restored", u64::from(layout_restored));
        if let Some(wgpu) = &cc.wgpu_render_state {
            let info = wgpu.adapter.get_info();
            core.metrics.environment.insert(
                "gpu".into(),
                format!("{} / {:?} / {}", info.name, info.backend, info.driver_info),
            );
        }
        for p in paths {
            core.open(&p);
        }
        if let Some(path) = saved_active.filter(|p| core.editors.contains_key(p)) {
            core.active = Some(path.clone());
            if let Some(tab) = dock.find_tab(&Tab::Editor(path)) {
                let _ = dock.set_active_tab(tab);
            }
        }
        core.begin_index(&cc.egui_ctx);
        let args: Vec<_> = std::env::args().collect();
        let qa = crate::arg(&args, "--qa-dir").map(PathBuf::from);
        let exercise = args.iter().any(|s| s == "--exercise");
        Self {
            core,
            dock,
            started,
            telemetry,
            first: true,
            ready_requested: false,
            close_prompt: false,
            qa,
            qa_step: 0,
            qa_next: Instant::now() + Duration::from_secs(2),
            exercise,
            last_frame: Instant::now(),
        }
    }
    fn persist(&mut self) {
        if !self.core.root.is_dir() {
            self.core.status = "Layout not saved: project directory is unavailable".into();
            return;
        }
        if self.qa.is_some() || self.exercise || std::env::args().any(|s| s == "--fresh-layout") {
            return;
        }
        let path = self.core.root.join(".nova");
        let result = (|| -> Result<(), String> {
            std::fs::create_dir_all(&path).map_err(|e| e.to_string())?;
            let data = ron::ser::to_string_pretty(
                &Workspace {
                    dock: self.dock.clone(),
                    nav: self.core.nav.clone(),
                    dark: self.core.dark,
                    thumb_size: self.core.thumb_size,
                    active: self.core.active.clone(),
                },
                ron::ser::PrettyConfig::default(),
            )
            .map_err(|e| e.to_string())?;
            std::fs::write(path.join("egui-layout.ron"), data).map_err(|e| e.to_string())
        })();
        self.core.status = match result {
            Ok(()) => "Workspace saved".into(),
            Err(e) => format!("Layout save failed: {e}"),
        };
    }
    fn show_panel(&mut self, tab: Tab) {
        if let Some(path) = self.dock.find_tab(&tab) {
            let _ = self.dock.set_active_tab(path);
        } else {
            self.dock.push_to_first_leaf(tab);
        }
    }
    fn qa_tick(&mut self, ctx: &egui::Context) {
        for event in ctx.input(|i| i.raw.events.clone()) {
            if let egui::Event::Screenshot {
                user_data, image, ..
            } = event
            {
                if user_data
                    .data
                    .as_ref()
                    .and_then(|v| v.downcast_ref::<String>())
                    .is_some_and(|name| name == "__ready")
                {
                    self.core.metrics.samples_ms.insert(
                        "ready_capture_ms".into(),
                        vec![self.started.elapsed().as_secs_f64() * 1000.],
                    );
                    if let Some(path) = &self.telemetry {
                        let _ = self.core.metrics.write(path);
                    }
                }
                if let (Some(dir), Some(name)) = (
                    &self.qa,
                    user_data
                        .data
                        .as_ref()
                        .and_then(|v| v.downcast_ref::<String>()),
                ) {
                    if name == "__ready" {
                        continue;
                    }
                    let _ = std::fs::create_dir_all(dir);
                    let bytes: Vec<u8> = image.pixels.iter().flat_map(|p| p.to_array()).collect();
                    if let Some(im) = image::RgbaImage::from_raw(
                        image.size[0] as u32,
                        image.size[1] as u32,
                        bytes,
                    ) && let Err(e) = im.save(dir.join(name))
                    {
                        self.core.status = e.to_string();
                    }
                }
            }
        }
        if self.qa.is_some() && Instant::now() >= self.qa_next && !self.core.editors.is_empty() {
            let names = [
                "01-default.png",
                "02-tree.png",
                "03-grid.png",
                "04-high-zoom.png",
                "05-preview.png",
                "06-rearranged.png",
                "07-compact.png",
            ];
            if self.qa_step.is_multiple_of(2) {
                let stage = self.qa_step / 2;
                match stage {
                    0 => {
                        if let Some(relative) = self.core.index.files.get(1).cloned() {
                            let original = self.core.active.clone();
                            let path = self.core.root.join(relative);
                            self.core.open(&path);
                            if let Some((node, _)) =
                                self.dock.main_surface().find_tab(&Tab::Welcome)
                            {
                                self.dock.main_surface_mut().set_focused_node(node);
                            }
                            self.dock.push_to_focused_leaf(Tab::Editor(path));
                            if let Some(original) = original {
                                self.core.active = Some(original.clone());
                                if let Some(tab) =
                                    self.dock.find_tab(&Tab::Editor(original.clone()))
                                {
                                    let _ = self.dock.set_active_tab(tab);
                                }
                                self.core.nav.selected = original
                                    .strip_prefix(&self.core.root)
                                    .ok()
                                    .map(Path::to_path_buf);
                            }
                        }
                    }
                    1 => {
                        self.core.nav.grid = false;
                        self.core.nav.directory = PathBuf::new();
                        for d in self.core.index.directories.keys() {
                            self.core.expanded.insert(d.clone());
                        }
                        self.core.refresh_rows = true;
                    }
                    2 => {
                        self.core.nav.grid = true;
                        if let Some(p) = self.core.nav.selected.clone() {
                            self.core.nav.directory = p.parent().unwrap_or(Path::new("")).into();
                        }
                    }
                    3 => {
                        if let Some(e) = self
                            .core
                            .active
                            .as_ref()
                            .and_then(|p| self.core.editors.get_mut(p))
                        {
                            e.zoom = 40.;
                            e.doc.draw([2, 2], [12, 10], [130, 141, 255, 255]);
                            e.doc.finish();
                        }
                    }
                    4 => {
                        self.core.angles = [-0.65, 1.1];
                        self.core.cube_scale = 1.7;
                    }
                    5 => {
                        let mut d = DockState::new(
                            self.core
                                .editors
                                .keys()
                                .map(|p| Tab::Editor(p.clone()))
                                .collect(),
                        );
                        let [center, right] = d.main_surface_mut().split_right(
                            NodeIndex::root(),
                            0.72,
                            vec![Tab::Assets],
                        );
                        d.main_surface_mut()
                            .split_below(right, 0.6, vec![Tab::Inspector]);
                        d.main_surface_mut()
                            .split_below(center, 0.65, vec![Tab::Preview]);
                        let floating = d.add_window(vec![Tab::Welcome]);
                        if let Some(w) = d.get_window_state_mut(floating) {
                            w.set_position(Pos2::new(450., 350.));
                            w.set_size(Vec2::new(260., 170.));
                        }
                        self.core.cube_scale = 1.;
                        self.dock = d;
                    }
                    6 => {
                        self.dock = default_layout();
                        if let Some((node, _)) = self.dock.main_surface().find_tab(&Tab::Welcome) {
                            self.dock.main_surface_mut().set_focused_node(node);
                        }
                        for p in self.core.editors.keys() {
                            self.dock.push_to_focused_leaf(Tab::Editor(p.clone()));
                        }
                        for e in self.core.editors.values_mut() {
                            e.zoom = 16.;
                        }
                        if let Some(path) = &self.core.active
                            && let Some(tab) = self.dock.find_tab(&Tab::Editor(path.clone()))
                        {
                            let _ = self.dock.set_active_tab(tab);
                        }
                        ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(Vec2::new(
                            960., 640.,
                        )));
                    }
                    _ => {}
                }
            } else {
                let stage = self.qa_step / 2;
                if stage < names.len() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::new(
                        names[stage].to_string(),
                    )));
                }
            }
            self.qa_step += 1;
            self.qa_next = Instant::now() + Duration::from_millis(450);
            if self.qa_step == 15 {
                for e in self.core.editors.values_mut() {
                    while e.doc.modified() {
                        e.doc.undo();
                    }
                }
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
        }
        if self.qa.is_some() {
            ctx.request_repaint_after(Duration::from_millis(30));
        }
        if self.exercise && !self.core.editors.is_empty() {
            let seconds = self.started.elapsed().as_secs_f64();
            let rate = if seconds < 4.0 {
                60
            } else if seconds < 8.0 {
                120
            } else {
                180
            };
            let interval = Duration::from_secs_f64(1.0 / rate as f64);
            if self.last_frame.elapsed() < interval {
                ctx.request_repaint_after(interval - self.last_frame.elapsed().min(interval));
                return;
            }
            let ms = self.last_frame.elapsed().as_secs_f64() * 1000.;
            self.last_frame = Instant::now();
            self.core
                .metrics
                .samples_ms
                .entry(format!("exercise_interval_{rate}"))
                .or_default()
                .push(ms);
            let i = self
                .core
                .metrics
                .counters
                .entry("exercise_frames".into())
                .or_default();
            *i += 1;
            let i = *i as u32;
            if let Some(e) = self
                .core
                .active
                .as_ref()
                .and_then(|p| self.core.editors.get_mut(p))
            {
                let w = e.doc.image.width();
                let h = e.doc.image.height();
                let p = [i % w, (i / w) % h];
                let t = Instant::now();
                e.doc.draw(p, p, [(i % 255) as u8, 130, 210, 255]);
                e.doc.finish();
                self.core.metrics.record("brush_event_cpu", t);
                if i.is_multiple_of(10) {
                    e.doc.undo();
                    e.doc.redo();
                }
                e.zoom = 12. + (i as f32 * 0.1).sin() * 5.;
            }
            self.core.nav.grid = true;
            self.core.angles[1] += 0.02;
            if i.is_multiple_of(40) {
                ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(Vec2::new(
                    1100. + (i % 200) as f32,
                    760.,
                )));
            }
            if seconds >= 12.0 {
                for e in self.core.editors.values_mut() {
                    e.doc = Document::new(e.doc.path.clone(), e.doc.image.clone());
                }
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            } else {
                ctx.request_repaint_after(Duration::from_secs_f64(1.0 / rate as f64));
            }
        }
    }
}
impl TabViewer for Core {
    type Tab = Tab;
    fn id(&mut self, tab: &mut Tab) -> egui::Id {
        egui::Id::new(tab)
    }
    fn on_tab_button(&mut self, tab: &mut Tab, response: &egui::Response) {
        if (response.clicked() || response.drag_started())
            && let Tab::Editor(path) = tab
        {
            self.active = Some(path.clone());
        }
    }
    fn title(&mut self, tab: &mut Tab) -> egui::WidgetText {
        match tab {
            Tab::Assets => "Assets".into(),
            Tab::Preview => "3D Preview".into(),
            Tab::Inspector => "Inspector".into(),
            Tab::Welcome => "Workspace".into(),
            Tab::Editor(p) => format!(
                "{}{}",
                p.file_name().unwrap_or_default().to_string_lossy(),
                if self.editors.get(p).is_some_and(|e| e.doc.modified()) {
                    " *"
                } else {
                    ""
                }
            )
            .into(),
        }
    }
    fn ui(&mut self, ui: &mut egui::Ui, tab: &mut Tab) {
        match tab {
            Tab::Assets => self.browser(ui),
            Tab::Editor(path) => self.canvas(ui, path),
            Tab::Inspector => self.inspector(ui),
            Tab::Welcome => {
                ui.heading("Nova Studio");
                ui.label("Open a project directory above, then double-click a PNG.");
                ui.label("Native pixels. A focused workspace.");
            }
            Tab::Preview => {
                if let Some(e) = self.active.as_ref().and_then(|p| self.editors.get_mut(p)) {
                    e.sync(ui.ctx(), &mut self.metrics);
                    let t = Instant::now();
                    render::cube(
                        ui,
                        e.texture.as_ref().unwrap().id(),
                        &mut self.angles,
                        &mut self.cube_scale,
                    );
                    self.metrics.record("preview_mesh_cpu", t);
                } else {
                    ui.label("Open a texture to preview.");
                }
            }
        }
    }
    fn on_close(&mut self, tab: &mut Tab) -> OnCloseResponse {
        if let Tab::Editor(p) = tab {
            if self.editors.get(p).is_some_and(|e| e.doc.modified()) {
                self.status = "Save this document before closing its tab".into();
                return OnCloseResponse::Ignore;
            }
            self.editors.remove(p);
            if self.active.as_ref() == Some(p) {
                self.active = self.editors.keys().next().cloned();
            }
        }
        OnCloseResponse::Close
    }
}
impl eframe::App for Nova {
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        let frame = Instant::now();
        let ctx = ui.ctx().clone();
        self.core.poll(&ctx);
        self.qa_tick(&ctx);
        ctx.set_visuals(if self.core.dark {
            egui::Visuals::dark()
        } else {
            egui::Visuals::light()
        });
        ctx.style_mut_of(
            if self.core.dark {
                egui::Theme::Dark
            } else {
                egui::Theme::Light
            },
            |s| {
                s.spacing.item_spacing = Vec2::new(6., 4.);
                s.visuals.selection.bg_fill = Color32::from_rgb(66, 72, 121);
                s.animation_time = 0.0;
            },
        );
        egui::Panel::top("commands").show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(
                    egui::RichText::new("*  NOVA STUDIO")
                        .strong()
                        .color(Color32::from_rgb(164, 173, 255)),
                );
                ui.separator();
                if ui.button("Save").clicked() {
                    self.core.action("save");
                }
                if ui.button("Undo").clicked() {
                    self.core.action("undo");
                }
                if ui.button("Redo").clicked() {
                    self.core.action("redo");
                }
                ui.menu_button("View", |ui| {
                    for (name, tab) in [
                        ("Assets", Tab::Assets),
                        ("Inspector", Tab::Inspector),
                        ("3D Preview", Tab::Preview),
                    ] {
                        if ui.button(name).clicked() {
                            self.show_panel(tab);
                            ui.close();
                        }
                    }
                    if ui.button("Reset layout").clicked() {
                        let paths: Vec<_> = self.core.editors.keys().cloned().collect();
                        self.dock = default_layout();
                        for p in paths {
                            self.dock.push_to_first_leaf(Tab::Editor(p));
                        }
                        ui.close();
                    }
                    if ui.button("Save workspace").clicked() {
                        self.persist();
                        ui.close();
                    }
                    ui.checkbox(&mut self.core.dark, "Dark theme");
                });
                ui.label(egui::RichText::new("PHASE 1 / EGUI + WGPU").small().weak());
            });
            ui.horizontal(|ui| {
                ui.label("Project");
                ui.add(
                    egui::TextEdit::singleline(&mut self.core.path_text)
                        .desired_width((ui.available_width() - 80.).max(80.)),
                );
                if ui.button("Open").clicked() {
                    if self.core.editors.values().any(|e| e.doc.modified()) {
                        self.core.status =
                            "Save modified documents before switching projects".into();
                    } else {
                        let root = PathBuf::from(&self.core.path_text);
                        if root.is_dir() {
                            self.core.root = root;
                            self.core.nav = Navigation::default();
                            self.core.index = Index::default();
                            self.core.editors.clear();
                            self.core.active = None;
                            self.core.thumbs.clear();
                            self.core.lru.clear();
                            self.core.begin_index(&ctx);
                            self.dock = default_layout();
                        } else {
                            self.core.status = "Project directory does not exist".into();
                        }
                    }
                }
            });
        });
        egui::Panel::bottom("status").show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(egui::RichText::new(&self.core.status).small());
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label(
                        egui::RichText::new(format!(
                            "{} assets · {} GPU thumbnails",
                            self.core.index.files.len(),
                            self.core.thumbs.len()
                        ))
                        .small()
                        .weak(),
                    );
                });
            });
        });
        if !ctx.text_edit_focused() {
            for (key, action) in [
                (egui::Key::S, "save"),
                (egui::Key::Z, "undo"),
                (egui::Key::Y, "redo"),
            ] {
                if ctx.input(|i| i.modifiers.command && i.key_pressed(key)) {
                    self.core.action(action);
                }
            }
            if ctx.input(|i| i.key_pressed(egui::Key::B)) {
                self.core.eraser = false;
            }
            if ctx.input(|i| i.key_pressed(egui::Key::E)) {
                self.core.eraser = true;
            }
        }
        egui::CentralPanel::default().show(ui, |ui| {
            DockArea::new(&mut self.dock)
                .style(egui_dock::Style::from_egui(ui.style()))
                .show_inside(ui, &mut self.core);
        });
        if let Some(path) = self.core.open_request.take() {
            self.core.open(&path);
            if self.core.editors.contains_key(&path) {
                let tab = Tab::Editor(path);
                if let Some(found) = self.dock.find_tab(&tab) {
                    let _ = self.dock.set_active_tab(found);
                } else {
                    if let Some((node, _)) = self.dock.main_surface().find_tab(&Tab::Welcome) {
                        self.dock.main_surface_mut().set_focused_node(node);
                    }
                    self.dock.push_to_focused_leaf(tab);
                }
                ctx.request_repaint();
            }
        }
        if ctx.input(|i| i.viewport().close_requested())
            && self.core.editors.values().any(|e| e.doc.modified())
        {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.close_prompt = true;
        }
        if self.close_prompt {
            egui::Window::new("Unsaved textures")
                .collapsible(false)
                .resizable(false)
                .show(&ctx, |ui| {
                    ui.label("Save modified textures before exiting?");
                    ui.horizontal(|ui| {
                        if ui.button("Save all and exit").clicked() {
                            let mut errors = vec![];
                            for e in self.core.editors.values_mut() {
                                if e.doc.modified()
                                    && let Err(error) = e.doc.save()
                                {
                                    errors.push(error);
                                }
                            }
                            if errors.is_empty() {
                                self.close_prompt = false;
                                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                            } else {
                                self.core.status = errors.join("; ");
                            }
                        }
                        if ui.button("Discard and exit").clicked() {
                            for e in self.core.editors.values_mut() {
                                e.doc = Document::new(e.doc.path.clone(), e.doc.image.clone());
                            }
                            self.close_prompt = false;
                            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                        }
                        if ui.button("Cancel").clicked() {
                            self.close_prompt = false;
                        }
                    });
                });
        }
        self.core.metrics.record("ui_build_cpu", frame);
        if !self.ready_requested
            && self.telemetry.is_some()
            && self.core.index_rx.is_none()
            && !self.core.editors.is_empty()
        {
            self.ready_requested = true;
            ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::new(
                "__ready".to_string(),
            )));
        }
        if self.first {
            self.first = false;
            self.core.metrics.samples_ms.insert(
                "first_ui_ms".into(),
                vec![self.started.elapsed().as_secs_f64() * 1000.0],
            );
            if let Some(path) = &self.telemetry {
                let _ = self.core.metrics.write(path);
            }
        }
    }
    fn on_exit(&mut self) {
        self.persist();
        if let Some(path) = &self.telemetry {
            let _ = self.core.metrics.write(path);
        }
    }
}
impl Core {
    fn begin_index(&mut self, ctx: &egui::Context) {
        let root = self.root.clone();
        let (tx, rx) = mpsc::channel();
        self.index_rx = Some(rx);
        let ctx = ctx.clone();
        self.status = "Indexing in background…".into();
        std::thread::spawn(move || {
            let t = Instant::now();
            let result =
                Index::scan(&root).map(|index| (index, t.elapsed().as_secs_f64() * 1000.0));
            let _ = tx.send(result);
            ctx.request_repaint();
        });
    }
    fn open(&mut self, path: &Path) {
        if !self.editors.contains_key(path) {
            match Document::open(path) {
                Ok(doc) => {
                    let zoom = (384.0 / doc.image.width() as f32).clamp(0.25, 24.0);
                    self.editors.insert(
                        path.into(),
                        Editor {
                            doc,
                            texture: None,
                            zoom,
                            pan: Vec2::ZERO,
                            last: None,
                            drawing: false,
                        },
                    );
                }
                Err(e) => {
                    self.status = e;
                    return;
                }
            }
        }
        self.active = Some(path.into());
        self.nav.selected = path.strip_prefix(&self.root).ok().map(Path::to_path_buf);
    }
    fn action(&mut self, action: &str) {
        if let Some(editor) = self.active.as_ref().and_then(|p| self.editors.get_mut(p)) {
            let t = Instant::now();
            match action {
                "save" => {
                    self.status = match editor.doc.save() {
                        Ok(()) => "PNG saved atomically".into(),
                        Err(e) => format!("Save failed: {e}"),
                    }
                }
                "undo" => editor.doc.undo(),
                "redo" => editor.doc.redo(),
                _ => {}
            }
            self.metrics.record(action, t);
        }
    }
    fn poll(&mut self, ctx: &egui::Context) {
        if let Some(result) = self.index_rx.as_ref().and_then(|rx| rx.try_recv().ok()) {
            self.index_rx = None;
            match result {
                Ok((index, ms)) => {
                    self.metrics
                        .samples_ms
                        .entry("index".into())
                        .or_default()
                        .push(ms);
                    self.status = format!("{} textures indexed in {:.1} ms", index.files.len(), ms);
                    self.index = index;
                    self.refresh_rows = true;
                    if self.editors.is_empty()
                        && let Some(p) = self.index.files.first()
                    {
                        let p = p.clone();
                        self.nav.directory = p.parent().unwrap_or(Path::new("")).to_path_buf();
                        self.open_request = Some(self.root.join(p));
                    }
                }
                Err(e) => self.status = e,
            }
        }
        for _ in 0..8 {
            let Ok((path, result)) = self.thumb_rx.try_recv() else {
                break;
            };
            self.pending.remove(&path);
            match result {
                Ok(im) => {
                    let tex = ctx.load_texture(
                        path.to_string_lossy(),
                        egui::ColorImage::from_rgba_unmultiplied([64, 64], im.as_raw()),
                        TextureOptions::NEAREST,
                    );
                    self.thumbs.insert(path.clone(), tex);
                    self.lru.push_back(path);
                    while self.lru.len() > 256 {
                        if let Some(old) = self.lru.pop_front() {
                            self.thumbs.remove(&old);
                        }
                    }
                }
                Err(_) => {
                    self.failed.insert(path);
                }
            }
            ctx.request_repaint();
        }
    }
    fn thumbnail(&mut self, ctx: &egui::Context, relative: &Path) -> Option<egui::TextureId> {
        let path = self.root.join(relative);
        if let Some(t) = self.thumbs.get(&path) {
            let id = t.id();
            self.lru.retain(|p| p != &path);
            self.lru.push_back(path);
            return Some(id);
        }
        if !self.failed.contains(&path)
            && !self.pending.contains(&path)
            && self.pending.len() < 128
            && self.thumb_tx.try_send(path.clone()).is_ok()
        {
            self.pending.insert(path);
            ctx.request_repaint();
        }
        None
    }
    fn rebuild_rows(&mut self) {
        self.rows.clear();
        if !self.nav.query.is_empty() {
            for p in self.nav.entries(&self.index) {
                self.rows.push((p, 0, false));
            }
        } else {
            fn walk(
                index: &Index,
                expanded: &BTreeSet<PathBuf>,
                parent: &Path,
                depth: usize,
                rows: &mut Vec<(PathBuf, usize, bool)>,
            ) {
                if let Some(entries) = index.directories.get(parent) {
                    let mut entries = entries.clone();
                    entries.sort_by_key(|p| (!index.directories.contains_key(p), p.clone()));
                    for p in entries {
                        let dir = index.directories.contains_key(&p);
                        rows.push((p.clone(), depth, dir));
                        if dir && expanded.contains(&p) {
                            walk(index, expanded, &p, depth + 1, rows);
                        }
                    }
                }
            }
            walk(
                &self.index,
                &self.expanded,
                &self.nav.directory,
                0,
                &mut self.rows,
            );
            if self.nav.reverse {
                self.rows.reverse();
            }
        }
        self.refresh_rows = false;
    }
    fn select(&mut self, path: PathBuf, open: bool) {
        self.nav.selected = Some(path.clone());
        if open {
            if self.index.directories.contains_key(&path) {
                self.nav.directory = path;
                self.refresh_rows = true;
            } else {
                self.open_request = Some(self.root.join(path));
            }
        }
    }
    fn browser(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            ui.strong("ASSETS");
            ui.selectable_value(&mut self.nav.grid, false, "Tree");
            ui.selectable_value(&mut self.nav.grid, true, "Grid");
        });
        if ui
            .add(
                egui::TextEdit::singleline(&mut self.nav.query)
                    .hint_text("Search textures…")
                    .desired_width(f32::INFINITY),
            )
            .changed()
        {
            self.refresh_rows = true;
        }
        ui.horizontal(|ui| {
            if ui.button("Up").on_hover_text("Parent directory").clicked() {
                self.nav.directory.pop();
                self.refresh_rows = true;
            }
            ui.label(
                egui::RichText::new(if self.nav.directory.as_os_str().is_empty() {
                    "Project".into()
                } else {
                    self.nav
                        .directory
                        .file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                })
                .small(),
            );
            if ui
                .small_button(if self.nav.reverse { "Z–A" } else { "A–Z" })
                .clicked()
            {
                self.nav.reverse = !self.nav.reverse;
                self.refresh_rows = true;
            }
        });
        ui.separator();
        if self.refresh_rows {
            self.rebuild_rows();
        }
        if self.nav.grid {
            ui.add(egui::Slider::new(&mut self.thumb_size, 48.0..=128.0).text("px"));
            let entries = self.nav.entries(&self.index);
            let cell = self.thumb_size + 16.0;
            let cols = (ui.available_width() / cell).floor().max(1.0) as usize;
            let rows = entries.len().div_ceil(cols);
            egui::ScrollArea::vertical()
                .id_salt("grid-scroll")
                .show_rows(ui, cell + 28.0, rows, |ui, range| {
                    for row in range {
                        ui.horizontal(|ui| {
                            for p in entries.iter().skip(row * cols).take(cols) {
                                let dir = self.index.directories.contains_key(p);
                                ui.vertical(|ui| {
                                    ui.set_width(cell - 6.0);
                                    let (rect, r) = ui.allocate_exact_size(
                                        Vec2::splat(self.thumb_size),
                                        egui::Sense::click(),
                                    );
                                    if self.nav.selected.as_ref() == Some(p) {
                                        ui.painter().rect_filled(
                                            rect.expand(3.0),
                                            3,
                                            Color32::from_rgb(67, 71, 116),
                                        );
                                    }
                                    if dir {
                                        ui.painter().text(
                                            rect.center(),
                                            egui::Align2::CENTER_CENTER,
                                            "DIR",
                                            egui::FontId::proportional(32.0),
                                            Color32::from_rgb(155, 168, 230),
                                        );
                                    } else if let Some(tex) = self.thumbnail(ui.ctx(), p) {
                                        render::checker(ui.painter(), rect);
                                        ui.painter().image(
                                            tex,
                                            rect,
                                            Rect::from_min_max(Pos2::ZERO, Pos2::new(1., 1.)),
                                            Color32::WHITE,
                                        );
                                    }
                                    if r.clicked() || r.double_clicked() {
                                        self.select(p.clone(), r.double_clicked());
                                    }
                                    r.on_hover_text(p.to_string_lossy());
                                    ui.add(
                                        egui::Label::new(
                                            egui::RichText::new(
                                                p.file_stem().unwrap_or_default().to_string_lossy(),
                                            )
                                            .small(),
                                        )
                                        .truncate(),
                                    );
                                });
                            }
                        });
                    }
                });
        } else {
            let rows = self.rows.clone();
            egui::ScrollArea::vertical()
                .id_salt("tree-scroll")
                .show_rows(ui, 23.0, rows.len(), |ui, range| {
                    for i in range {
                        let (p, depth, dir) = &rows[i];
                        ui.horizontal(|ui| {
                            ui.add_space(*depth as f32 * 13.0);
                            if *dir {
                                if ui
                                    .small_button(if self.expanded.contains(p) { "v" } else { ">" })
                                    .clicked()
                                {
                                    if !self.expanded.remove(p) {
                                        self.expanded.insert(p.clone());
                                    }
                                    self.refresh_rows = true;
                                }
                            } else {
                                ui.label(
                                    egui::RichText::new("[]")
                                        .color(Color32::from_rgb(145, 155, 225)),
                                );
                            }
                            let r = ui.selectable_label(
                                self.nav.selected.as_ref() == Some(p),
                                p.file_name().unwrap_or_default().to_string_lossy(),
                            );
                            if r.clicked() || r.double_clicked() {
                                self.select(p.clone(), r.double_clicked());
                            }
                            r.on_hover_text(p.to_string_lossy());
                        });
                    }
                });
        }
        if ui.rect_contains_pointer(ui.max_rect()) && !ui.ctx().text_edit_focused() {
            let entries = if self.nav.grid {
                self.nav.entries(&self.index)
            } else {
                self.rows.iter().map(|r| r.0.clone()).collect()
            };
            let current = self
                .nav
                .selected
                .as_ref()
                .and_then(|p| entries.iter().position(|x| x == p))
                .unwrap_or(0);
            if ui.input(|i| i.key_pressed(egui::Key::ArrowDown)) && !entries.is_empty() {
                self.select(entries[(current + 1).min(entries.len() - 1)].clone(), false);
            }
            if ui.input(|i| i.key_pressed(egui::Key::ArrowUp)) && !entries.is_empty() {
                self.select(entries[current.saturating_sub(1)].clone(), false);
            }
            if ui.input(|i| i.key_pressed(egui::Key::Enter))
                && let Some(p) = self.nav.selected.clone()
            {
                self.select(p, true);
            }
        }
    }
    fn canvas(&mut self, ui: &mut egui::Ui, path: &Path) {
        let Some(e) = self.editors.get_mut(path) else {
            ui.label("File unavailable. Reopen from Assets.");
            return;
        };
        ui.horizontal_wrapped(|ui| {
            ui.label(format!(
                "{} × {}  RGBA",
                e.doc.image.width(),
                e.doc.image.height()
            ));
            ui.separator();
            if ui.small_button("−").clicked() {
                e.zoom = (e.zoom / 1.25).max(0.25);
            }
            ui.label(format!("{:.0}%", e.zoom * 100.0));
            if ui.small_button("+").clicked() {
                e.zoom = (e.zoom * 1.25).min(64.0);
            }
            if ui.small_button("Center").clicked() {
                e.pan = Vec2::ZERO;
            }
            ui.checkbox(&mut self.grid, "Grid");
        });
        let (rect, r) = ui.allocate_exact_size(
            ui.available_size().max(Vec2::splat(20.0)),
            egui::Sense::click_and_drag(),
        );
        let size = Vec2::new(e.doc.image.width() as f32, e.doc.image.height() as f32);
        let mut origin = rect.center() - size * e.zoom * 0.5 + e.pan;
        if r.hovered() {
            let delta = ui.input(|i| i.smooth_scroll_delta.y);
            if delta != 0.0 {
                let old = e.zoom;
                e.zoom = crate::pixels::zoom(e.zoom, delta);
                if let Some(pointer) = r.hover_pos() {
                    e.pan += (pointer - rect.center() - e.pan) * (1.0 - e.zoom / old);
                }
                origin = rect.center() - size * e.zoom * 0.5 + e.pan;
            }
        }
        if r.dragged_by(egui::PointerButton::Middle) || r.dragged_by(egui::PointerButton::Secondary)
        {
            e.pan += ui.input(|i| i.pointer.delta());
            origin = rect.center() - size * e.zoom * 0.5 + e.pan;
        }
        for event in ui.input(|i| i.raw.events.clone()) {
            let pointer = match event {
                egui::Event::PointerButton {
                    pos,
                    button: egui::PointerButton::Primary,
                    pressed: true,
                    ..
                } if rect.contains(pos) => {
                    e.drawing = true;
                    self.active = Some(path.into());
                    Some(pos)
                }
                egui::Event::PointerMoved(pos) if e.drawing => Some(pos),
                egui::Event::PointerButton {
                    button: egui::PointerButton::Primary,
                    pressed: false,
                    ..
                } => {
                    e.doc.finish();
                    e.last = None;
                    e.drawing = false;
                    None
                }
                _ => None,
            };
            if let Some(pointer) = pointer {
                if let Some(p) = crate::pixels::coordinate(
                    [pointer.x, pointer.y],
                    [origin.x, origin.y],
                    e.zoom,
                    [e.doc.image.width(), e.doc.image.height()],
                ) {
                    let t = Instant::now();
                    e.doc.draw(
                        e.last.unwrap_or(p),
                        p,
                        if self.eraser { [0; 4] } else { self.color },
                    );
                    e.last = Some(p);
                    self.metrics.record("brush_event_cpu", t);
                } else {
                    e.last = None;
                }
            }
        }
        e.sync(ui.ctx(), &mut self.metrics);
        let painter = ui.painter().with_clip_rect(rect);
        painter.rect_filled(rect, 0, Color32::from_rgb(21, 23, 30));
        let image_rect = Rect::from_min_size(origin, size * e.zoom);
        render::checker(&painter, image_rect);
        painter.image(
            e.texture.as_ref().unwrap().id(),
            image_rect,
            Rect::from_min_max(Pos2::ZERO, Pos2::new(1., 1.)),
            Color32::WHITE,
        );
        if self.grid && e.zoom >= 6.0 {
            let stroke = egui::Stroke::new(0.5, Color32::from_black_alpha(65));
            for x in 0..=e.doc.image.width() {
                let x = origin.x + x as f32 * e.zoom;
                painter.line_segment(
                    [Pos2::new(x, origin.y), Pos2::new(x, image_rect.bottom())],
                    stroke,
                );
            }
            for y in 0..=e.doc.image.height() {
                let y = origin.y + y as f32 * e.zoom;
                painter.line_segment(
                    [Pos2::new(origin.x, y), Pos2::new(image_rect.right(), y)],
                    stroke,
                );
            }
        }
        if r.hovered() {
            ui.ctx().set_cursor_icon(egui::CursorIcon::Crosshair);
        }
    }
    fn inspector(&mut self, ui: &mut egui::Ui) {
        egui::ScrollArea::vertical().show(ui,|ui|{ui.strong("PIXEL TOOLS");ui.add_space(8.0);ui.horizontal(|ui|{ui.selectable_value(&mut self.eraser,false,"Pencil B");ui.selectable_value(&mut self.eraser,true,"Eraser E");});ui.add_space(10.0);ui.label("Foreground · RGBA");ui.color_edit_button_srgba_unmultiplied(&mut self.color);ui.horizontal_wrapped(|ui|{for color in [[130,141,255,255],[170,122,225,255],[66,180,161,255],[233,182,103,255],[237,237,242,255],[35,38,49,255]]{if ui.add(egui::Button::new("   ").fill(Color32::from_rgba_unmultiplied(color[0],color[1],color[2],color[3]))).clicked(){self.color=color;}}});ui.separator();ui.strong("DOCUMENT");if let Some(e)=self.active.as_ref().and_then(|p|self.editors.get(p)){ui.label(e.doc.path.file_name().unwrap_or_default().to_string_lossy());ui.label(format!("{} × {} pixels",e.doc.image.width(),e.doc.image.height()));ui.label(if e.doc.modified(){"Modified"}else{"Saved"});}ui.separator();ui.label("Layers");ui.label(egui::RichText::new("Single RGBA layer · Phase 1").small().weak());ui.add_space(16.0);ui.label(egui::RichText::new("Ctrl+S   Save\nCtrl+Z   Undo\nCtrl+Y   Redo\nWheel    Zoom\nMiddle / right drag    Pan\nDrag panel tabs to rearrange").monospace().small());});
    }
}
