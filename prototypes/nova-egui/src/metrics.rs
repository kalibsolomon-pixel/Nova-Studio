use serde::Serialize;
use std::{collections::BTreeMap, path::Path, time::Instant};
#[derive(Default, Serialize)]
pub struct Metrics {
    pub samples_ms: BTreeMap<String, Vec<f64>>,
    pub counters: BTreeMap<String, u64>,
    pub environment: BTreeMap<String, String>,
}
impl Metrics {
    pub fn record(&mut self, name: &str, start: Instant) {
        let list = self.samples_ms.entry(name.into()).or_default();
        if list.len() < 20_000 {
            list.push(start.elapsed().as_secs_f64() * 1000.0);
        }
    }
    pub fn count(&mut self, name: &str, n: u64) {
        *self.counters.entry(name.into()).or_default() += n;
    }
    pub fn write(&self, path: &Path) -> Result<(), String> {
        std::fs::write(
            path,
            serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())
    }
}
pub fn bench(root: &Path, out: &Path) -> Result<(), String> {
    let mut m = Metrics::default();
    let mut index = crate::assets::Index::default();
    for _ in 0..5 {
        let t = Instant::now();
        index = crate::assets::Index::scan(root)?;
        m.record("index", t);
    }
    m.count("assets", index.files.len() as u64);
    for p in index.files.iter().take(200) {
        let t = Instant::now();
        let im = crate::io::load(&root.join(p))?;
        std::hint::black_box(image::imageops::resize(
            &im,
            64,
            64,
            image::imageops::FilterType::Nearest,
        ));
        m.record("thumbnail_decode_resize", t);
    }
    let temp = tempfile::tempdir().map_err(|e| e.to_string())?;
    let mut d = crate::document::Document::new(
        temp.path().join("save.png"),
        image::RgbaImage::new(256, 256),
    );
    for i in 0..1000 {
        let t = Instant::now();
        d.draw(
            [i % 256, (i / 256) % 256],
            [i % 256, (i / 256) % 256],
            [i as u8, 72, 180, 255],
        );
        d.finish();
        m.record("brush_command", t);
        let t = Instant::now();
        d.undo();
        m.record("undo", t);
        let t = Instant::now();
        d.redo();
        m.record("redo", t);
    }
    for _ in 0..30 {
        let t = Instant::now();
        d.save()?;
        m.record("save_256", t);
    }
    m.write(out)
}
