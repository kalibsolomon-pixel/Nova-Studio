use crate::{io, pixels};
use image::{Rgba, RgbaImage};
use std::{
    collections::BTreeMap,
    path::{Path, PathBuf},
};
#[derive(Clone)]
struct Change {
    index: u32,
    before: [u8; 4],
    after: [u8; 4],
}
pub struct Document {
    pub path: PathBuf,
    pub image: RgbaImage,
    history: Vec<Vec<Change>>,
    cursor: usize,
    saved: Option<usize>,
    stroke: BTreeMap<u32, Change>,
    pub dirty: Option<[u32; 4]>,
}
impl Document {
    pub fn open(path: &Path) -> io::Result<Self> {
        Ok(Self::new(path.to_path_buf(), io::load(path)?))
    }
    pub fn new(path: PathBuf, image: RgbaImage) -> Self {
        let (w, h) = image.dimensions();
        Self {
            path,
            image,
            history: vec![],
            cursor: 0,
            saved: Some(0),
            stroke: BTreeMap::new(),
            dirty: Some([0, 0, w, h]),
        }
    }
    pub fn modified(&self) -> bool {
        self.saved != Some(self.cursor) || !self.stroke.is_empty()
    }
    pub fn mark(&mut self, x: u32, y: u32) {
        self.dirty = Some(match self.dirty {
            Some([l, t, r, b]) => [l.min(x), t.min(y), r.max(x + 1), b.max(y + 1)],
            None => [x, y, x + 1, y + 1],
        });
    }
    pub fn draw(&mut self, a: [u32; 2], b: [u32; 2], color: [u8; 4]) {
        for [x, y] in pixels::line(a, b) {
            if x >= self.image.width() || y >= self.image.height() {
                continue;
            }
            let before = self.image.get_pixel(x, y).0;
            if before == color {
                continue;
            }
            let index = y * self.image.width() + x;
            self.stroke
                .entry(index)
                .and_modify(|c| c.after = color)
                .or_insert(Change {
                    index,
                    before,
                    after: color,
                });
            self.image.put_pixel(x, y, Rgba(color));
            self.mark(x, y);
        }
    }
    pub fn finish(&mut self) {
        if self.stroke.is_empty() {
            return;
        }
        if self.saved.is_some_and(|s| s > self.cursor) {
            self.saved = None;
        }
        self.history.truncate(self.cursor);
        self.history.push(
            std::mem::take(&mut self.stroke)
                .into_values()
                .filter(|c| c.before != c.after)
                .collect(),
        );
        self.cursor += 1;
    }
    pub fn undo(&mut self) {
        self.finish();
        if self.cursor == 0 {
            return;
        }
        self.cursor -= 1;
        self.apply(false);
    }
    pub fn redo(&mut self) {
        self.finish();
        if self.cursor == self.history.len() {
            return;
        }
        self.apply(true);
        self.cursor += 1;
    }
    fn apply(&mut self, forward: bool) {
        let width = self.image.width();
        for c in self.history[self.cursor].clone() {
            let (x, y) = (c.index % width, c.index / width);
            self.image
                .put_pixel(x, y, Rgba(if forward { c.after } else { c.before }));
            self.mark(x, y);
        }
    }
    pub fn save(&mut self) -> io::Result<()> {
        self.finish();
        io::save(&self.path, &self.image)?;
        self.saved = Some(self.cursor);
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn brush_erase_undo_and_save_branch() {
        let tmp = tempfile::tempdir().unwrap();
        let mut d = Document::new(tmp.path().join("t.png"), RgbaImage::new(16, 16));
        d.draw([0, 0], [15, 15], [20, 30, 40, 255]);
        d.finish();
        assert!(d.modified());
        assert_eq!(d.image.get_pixel(8, 8).0, [20, 30, 40, 255]);
        d.save().unwrap();
        assert!(!d.modified());
        d.draw([8, 8], [8, 8], [0, 0, 0, 0]);
        d.finish();
        d.undo();
        assert!(!d.modified());
        assert_eq!(d.image.get_pixel(8, 8).0, [20, 30, 40, 255]);
        d.redo();
        assert_eq!(d.image.get_pixel(8, 8).0, [0; 4]);
        d.undo();
        d.undo();
        d.draw([0, 0], [1, 0], [1; 4]);
        d.finish();
        assert!(d.modified());
        d.redo();
        assert_eq!(d.image.get_pixel(8, 8).0, [0; 4]);
    }
    #[test]
    fn repeated_pixel_undo_restores_original() {
        let mut d = Document::new(PathBuf::new(), RgbaImage::new(16, 16));
        d.draw([1, 1], [1, 1], [1; 4]);
        d.draw([1, 1], [1, 1], [2; 4]);
        d.finish();
        d.undo();
        assert_eq!(d.image.get_pixel(1, 1).0, [0; 4]);
        d.redo();
        assert_eq!(d.image.get_pixel(1, 1).0, [2; 4]);
    }
}
