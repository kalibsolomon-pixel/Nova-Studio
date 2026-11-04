use image::{ImageReader, RgbaImage};
use std::{io::Write, path::Path};
pub type Result<T> = std::result::Result<T, String>;
pub fn load(path: &Path) -> Result<RgbaImage> {
    if path
        .extension()
        .is_none_or(|x| !x.eq_ignore_ascii_case("png"))
    {
        return Err("Only PNG images are supported".into());
    }
    let mut reader = ImageReader::open(path).map_err(|e| e.to_string())?;
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(4096);
    limits.max_image_height = Some(4096);
    limits.max_alloc = Some(128 * 1024 * 1024);
    reader.limits(limits);
    reader
        .decode()
        .map(|im| im.to_rgba8())
        .map_err(|e| e.to_string())
}
pub fn save(path: &Path, image: &RgbaImage) -> Result<()> {
    let parent = path.parent().ok_or("Missing parent directory")?;
    let mut temp = tempfile::NamedTempFile::new_in(parent).map_err(|e| e.to_string())?;
    image
        .write_to(&mut temp, image::ImageFormat::Png)
        .map_err(|e| e.to_string())?;
    temp.flush().map_err(|e| e.to_string())?;
    temp.as_file().sync_all().map_err(|e| e.to_string())?;
    temp.persist(path).map_err(|e| e.to_string())?;
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rgba_roundtrip_and_failures() {
        let tmp = tempfile::tempdir().unwrap();
        for size in [16, 32, 64, 128, 256] {
            let image = RgbaImage::from_fn(size, size, |x, y| {
                image::Rgba([x as u8, y as u8, 91, if x % 2 == 0 { 0 } else { 123 }])
            });
            let path = tmp.path().join(format!("{size}.png"));
            save(&path, &image).unwrap();
            assert_eq!(load(&path).unwrap(), image);
            save(&path, &image).unwrap(); // replacement is required on Windows too
        }
        assert!(load(&tmp.path().join("missing.png")).is_err());
        assert!(save(&tmp.path().join("absent/file.png"), &RgbaImage::new(16, 16)).is_err());
        std::fs::write(tmp.path().join("bad.png"), b"not png").unwrap();
        assert!(load(&tmp.path().join("bad.png")).is_err());
    }
}
