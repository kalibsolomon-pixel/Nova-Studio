//! CPU transforms with GPU textured triangles in eframe's shared wgpu pass.
use eframe::egui::{self, Color32, Pos2, Rect, TextureId, Vec2};
pub fn cube(ui: &mut egui::Ui, texture: TextureId, angles: &mut [f32; 2], scale: &mut f32) {
    let (rect, response) = ui.allocate_exact_size(
        ui.available_size().max(Vec2::splat(40.0)),
        egui::Sense::drag(),
    );
    if response.dragged() {
        let d = ui.input(|i| i.pointer.delta());
        angles[0] += d.y * 0.01;
        angles[1] += d.x * 0.01;
    }
    if response.hovered() {
        *scale = crate::pixels::zoom(*scale, ui.input(|i| i.smooth_scroll_delta.y)).clamp(0.3, 2.0);
    }
    ui.painter()
        .rect_filled(rect, 0, Color32::from_rgb(19, 22, 30));
    let mut points = Vec::new();
    for [x, y, z] in [
        [-1., -1., -1.],
        [1., -1., -1.],
        [1., 1., -1.],
        [-1., 1., -1.],
        [-1., -1., 1.],
        [1., -1., 1.],
        [1., 1., 1.],
        [-1., 1., 1.],
    ] {
        let (sx, cx) = angles[0].sin_cos();
        let (sy, cy) = angles[1].sin_cos();
        let (x, z) = (x * cy + z * sy, -x * sy + z * cy);
        let (y, z) = (y * cx - z * sx, y * sx + z * cx);
        points.push([x, y, z]);
    }
    let mut faces = vec![
        ([0, 3, 2, 1], 0.72),
        ([4, 5, 6, 7], 0.9),
        ([0, 4, 7, 3], 0.65),
        ([1, 2, 6, 5], 0.8),
        ([0, 1, 5, 4], 1.0),
        ([3, 7, 6, 2], 0.55),
    ];
    faces.sort_by(|(a, _), (b, _)| {
        a.iter()
            .map(|i| points[*i][2])
            .sum::<f32>()
            .total_cmp(&b.iter().map(|i| points[*i][2]).sum::<f32>())
    });
    let radius = rect.width().min(rect.height()) * 0.25 * *scale;
    for (face, shade) in faces {
        let p: Vec<Pos2> = face
            .iter()
            .map(|i| rect.center() + Vec2::new(points[*i][0], points[*i][1]) * radius)
            .collect();
        let cross = (p[1] - p[0]).x * (p[2] - p[0]).y - (p[1] - p[0]).y * (p[2] - p[0]).x;
        if cross >= 0.0 {
            continue;
        }
        let mut mesh = egui::Mesh::with_texture(texture);
        let c = Color32::from_gray((255.0 * shade) as u8);
        for (pos, uv) in p.iter().zip([[0., 0.], [1., 0.], [1., 1.], [0., 1.]]) {
            mesh.vertices.push(egui::epaint::Vertex {
                pos: *pos,
                uv: Pos2::new(uv[0], uv[1]),
                color: c,
            });
        }
        mesh.indices = vec![0, 1, 2, 0, 2, 3];
        ui.painter().with_clip_rect(rect).add(mesh);
    }
    ui.painter().text(
        rect.left_top() + Vec2::splat(12.0),
        egui::Align2::LEFT_TOP,
        "CUBE PREVIEW · Drag to orbit / Scroll to zoom",
        egui::FontId::monospace(11.0),
        Color32::from_gray(165),
    );
}
pub fn checker(painter: &egui::Painter, rect: Rect) {
    let clip = rect.intersect(painter.clip_rect());
    if !clip.is_positive() {
        return;
    }
    let step = 12.0;
    let start = clip.min;
    for y in 0..(clip.height() / step).ceil() as i32 {
        for x in 0..(clip.width() / step).ceil() as i32 {
            let r = Rect::from_min_size(
                start + Vec2::new(x as f32 * step, y as f32 * step),
                Vec2::splat(step),
            )
            .intersect(clip);
            painter.rect_filled(
                r,
                0,
                if (x + y) % 2 == 0 {
                    Color32::from_gray(43)
                } else {
                    Color32::from_gray(53)
                },
            );
        }
    }
}
