//! Integer raster operations independent of the GUI and filesystem.
pub fn coordinate(
    point: [f32; 2],
    origin: [f32; 2],
    zoom: f32,
    size: [u32; 2],
) -> Option<[u32; 2]> {
    if !zoom.is_finite() || zoom <= 0.0 {
        return None;
    }
    let x = ((point[0] - origin[0]) / zoom).floor();
    let y = ((point[1] - origin[1]) / zoom).floor();
    (x >= 0.0 && y >= 0.0 && x < size[0] as f32 && y < size[1] as f32)
        .then_some([x as u32, y as u32])
}
pub fn zoom(current: f32, delta: f32) -> f32 {
    (current * 1.002_f32.powf(delta)).clamp(0.25, 64.0)
}
pub fn line(a: [u32; 2], b: [u32; 2]) -> Vec<[u32; 2]> {
    let (mut x, mut y) = (a[0] as i32, a[1] as i32);
    let (tx, ty) = (b[0] as i32, b[1] as i32);
    let dx = (tx - x).abs();
    let dy = -(ty - y).abs();
    let sx = if x < tx { 1 } else { -1 };
    let sy = if y < ty { 1 } else { -1 };
    let mut error = dx + dy;
    let mut result = Vec::new();
    loop {
        result.push([x as u32, y as u32]);
        if x == tx && y == ty {
            break;
        }
        let twice = 2 * error;
        if twice >= dy {
            error += dy;
            x += sx;
        }
        if twice <= dx {
            error += dx;
            y += sy;
        }
    }
    result
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn boundaries_and_fractional_zoom() {
        assert_eq!(coordinate([9.9, 10.0], [10.0, 10.0], 8.0, [16, 16]), None);
        assert_eq!(
            coordinate([137.9, 137.9], [10.0, 10.0], 8.0, [16, 16]),
            Some([15, 15])
        );
        assert_eq!(coordinate([138.0, 10.0], [10.0, 10.0], 8.0, [16, 16]), None);
        assert_eq!(
            coordinate([11.0, 11.0], [10.0, 10.0], 0.5, [16, 16]),
            Some([2, 2])
        );
        assert_eq!(coordinate([0.0, 0.0], [0.0, 0.0], 0.0, [16, 16]), None);
    }
    #[test]
    fn zoom_clamped_and_reversible() {
        assert!((zoom(zoom(8.0, 100.0), -100.0) - 8.0).abs() < 0.001);
        assert_eq!(zoom(64.0, 1000.0), 64.0);
    }
    #[test]
    fn continuous_lines_all_octants() {
        for end in [[0, 0], [10, 0], [0, 10], [10, 10], [3, 10]] {
            let points = line([5, 5], end);
            assert_eq!(points[0], [5, 5]);
            assert_eq!(*points.last().unwrap(), end);
            for p in points.windows(2) {
                assert!(p[0][0].abs_diff(p[1][0]) <= 1 && p[0][1].abs_diff(p[1][1]) <= 1);
            }
        }
    }
}
