// SPDX-License-Identifier: GPL-3.0-or-later
#include "cube.h"
#include "theme.h"
#include <QOpenGLFramebufferObject>
#include <QScopedValueRollback>
#include <QJsonDocument>
#include <QLabel>
#include <QTextLayout>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QWheelEvent>
#include <numeric>
namespace nova {
struct Vertex {
    float x, y, z, u, v, nx, ny, nz, shade;
};
Cube::Cube(Metrics *m, QWidget *p) : QOpenGLWidget(p), metrics(m) {
    setMinimumSize(200, 150);
    setFocusPolicy(Qt::StrongFocus);
    mc::Model model;
    model.valid = true;
    model.version = "26.1";
    model.resolved =
        QJsonDocument::fromJson(
            R"json({"elements":[{"from":[0,0,0],"to":[16,16,16],"faces":{"north":{"texture":"nova:active"},"south":{"texture":"nova:active"},"west":{"texture":"nova:active"},"east":{"texture":"nova:active"},"up":{"texture":"nova:active"},"down":{"texture":"nova:active"}}}]})json")
            .object();
    genericMesh = mc::mesh(model);
}
Cube::~Cube() {
    cleanup();
    if (context())
        disconnect(context(), nullptr, this, nullptr);
}
void Cube::cleanup() {
    if (!context())
        return;
    makeCurrent();
    buffer.destroy();
    lineBuffer.destroy();
    guideBuffer.destroy();
    if (texture)
        glDeleteTextures(1, &texture);
    for (auto t : materialTextures) {
        glDeleteTextures(1, &t);
        metrics->count("material_texture_releases", 1);
    }
    materialTextures.clear();
    materialSizes.clear();
    dirtyMaterials.clear();
    program.reset();
    texture = 0;
    textureSize = {};
    geometryDirty = materialsDirty = fullUpload = true;
    doneCurrent();
}
void Cube::setDocument(std::shared_ptr<Document> doc) {
    if (doc == document) {
        if (document && !document->dirty.isEmpty())
            update();
        return;
    }
    document = std::move(doc);
    playbackPreview = {};
    fullUpload = true;
    if (!document && texture && context() && context()->isValid()) {
        makeCurrent();
        glDeleteTextures(1, &texture);
        texture = 0;
        textureSize = {};
        metrics->count("texture_releases", 1);
        doneCurrent();
    }
    update();
}
void Cube::setModel(std::shared_ptr<const mc::Mesh> mesh, const QString &editable,
                    const QMap<QString, QImage> &materials) {
    if (modelMesh == mesh && activeTexture == editable)
        return;
    geometryDirty = modelMesh != mesh || geometryDirty;
    // Geometry edits rebuild the mesh even when it still uses the same images.
    // Retain existing uploads for identical pixels and keep the fallback image.
    const auto fallback = materialImages.value(QString{});
    bool materialSetChanged = materialImages.size() - (fallback.isNull() ? 0 : 1) != materials.size();
    for (auto it = materials.cbegin(); it != materials.cend(); ++it) {
        const auto previous = materialImages.constFind(it.key());
        if (previous == materialImages.cend())
            materialSetChanged = true;
        else if (previous->cacheKey() != it->cacheKey())
            dirtyMaterials.insert(it.key());
    }
    materialsDirty = materialsDirty || materialSetChanged;
    modelMesh = std::move(mesh);
    activeTexture = editable;
    materialImages = materials;
    if (!fallback.isNull())
        materialImages.insert(QString{}, fallback);
    alphaCache.clear();
    selectedFace = -1;
    update();
}
// The notice is a child label over the GL view rather than QPainter output in paintGL:
// painter output there reached only grabFramebuffer(), never the on-screen widget.
void Cube::setNotice(const QString &text) {
    if (noticeText == text)
        return;
    // The view's own tooltip carries the full notice (the banner lets the mouse through so
    // orbiting works); keep any other tooltip, such as a shader log, untouched.
    if (toolTip() == noticeText)
        setToolTip(text);
    noticeText = text;
    setAccessibleDescription(text);
    if (!noticeLabel) {
        noticeLabel = new QLabel(this);
        noticeLabel->setObjectName("previewNotice");
        noticeLabel->setWordWrap(false); // placeNotice() wraps within the line budget
        noticeLabel->setTextFormat(Qt::PlainText);
        noticeLabel->setAttribute(Qt::WA_TransparentForMouseEvents); // orbiting still works under it
        const auto &t = ui::current();
        noticeLabel->setStyleSheet(QString("QLabel#previewNotice{background:rgb(39,42,54);color:rgb(222,225,235);"
                                           "border:1px solid %1;border-left:3px solid %1;padding:%2px %3px}")
                                       .arg(t.warning.name()).arg(ui::SpaceS).arg(ui::SpaceL));
    }
    noticeLabel->setAccessibleName(text);
    noticeLabel->setVisible(!text.isEmpty());
    placeNotice();
}
QStringList Cube::noticeLines(const QString &text, const QFont &font, int width, int maxLines) {
    const QFontMetrics metrics(font);
    width = std::max(1, width);
    QStringList lines;
    const auto paragraphs = text.split('\n');
    for (int p = 0; p < paragraphs.size(); ++p) {
        const auto &paragraph = paragraphs[p];
        QTextLayout layout(paragraph, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        for (auto line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(width);
            if (int(lines.size()) == maxLines - 1) {
                // Last line in the budget: the rest of this paragraph and any later ones.
                QString rest = paragraph.mid(line.textStart());
                for (int q = p + 1; q < paragraphs.size(); ++q)
                    rest += ' ' + paragraphs[q];
                lines << metrics.elidedText(rest.simplified(), Qt::ElideRight, width);
                layout.endLayout();
                return lines;
            }
            lines << paragraph.mid(line.textStart(), line.textLength()).trimmed();
        }
        layout.endLayout();
    }
    return lines;
}
void Cube::placeNotice() {
    if (!noticeLabel || noticeText.isEmpty())
        return;
    const int width = std::max(80, this->width() - 2 * ui::SpaceL);
    // The label's own frame (style sheet padding and borders), measured on one character.
    const QFontMetrics metrics(noticeLabel->font());
    noticeLabel->setText("M");
    const QSize frame = noticeLabel->sizeHint() - QSize(metrics.horizontalAdvance('M'), metrics.lineSpacing());
    const int lineHeight = metrics.lineSpacing();
    // At most a quarter of the view (at least one line), and never more than four lines.
    const int budget = std::clamp(int((height() * NoticeMaxShare - frame.height()) / lineHeight), 1, 4);
    const int textWidth = width - frame.width() - 2;
    const auto lines = noticeLines(noticeText, noticeLabel->font(), textWidth, budget);
    noticeLabel->setText(lines.join('\n'));
    noticeLabel->setProperty("elided", lines.join('\n').remove('\n').remove(' ') !=
                                           QString(noticeText).remove('\n').remove(' '));
    noticeLabel->setGeometry(ui::SpaceL, ui::SpaceL, width, noticeLabel->sizeHint().height());
    noticeLabel->raise();
    if (toolTip().isEmpty())
        setToolTip(noticeText);
}
void Cube::changeEvent(QEvent *event) {
    QOpenGLWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        placeNotice();
}
void Cube::resizeEvent(QResizeEvent *event) {
    QOpenGLWidget::resizeEvent(event);
    placeNotice();
}
void Cube::setMaterialImage(const QString &asset, const QImage &image) {
    if (image.isNull() || materialImages.value(asset).cacheKey() == image.cacheKey())
        return;
    materialImages[asset] = image;
    dirtyMaterials.insert(asset);
    update();
}
void Cube::showEvent(QShowEvent *event) {
    QOpenGLWidget::showEvent(event);
    emit visibilityChanged(true);
}
void Cube::hideEvent(QHideEvent *event) {
    emit visibilityChanged(false);
    QOpenGLWidget::hideEvent(event);
}
void Cube::resetView() {
    pitch = 25;
    yaw = 38;
    cameraPan = {};
    fitModel();
}
void Cube::setContextFraming(bool enabled, bool frontView) {
    contextFraming = enabled;
    if (enabled) {
        // Fixed camera: a one-block reference fills a constant share of the view and the
        // model is not re-centred, so display scale and translation stay visible.
        // GUI items are drawn facing the viewer; other contexts use a 3/4 view so the
        // block reference reads as a cube.
        pitch = frontView ? 0.f : 25.f;
        yaw = frontView ? 0.f : 38.f;
        cameraPan = {};
        scale = 1;
    }
    update();
}
void Cube::fitModel() {
    if (contextFraming) {
        scale = 1;
        cameraPan = {};
        update();
        return;
    }
    float span = (scene().maximum - scene().minimum).length();
    float factor = 3.4641f;
    if (perspective) {
        const float aspect = float(width()) / float(std::max(1, height()));
        const float halfAngle = std::atan(std::tan(.34906585f) * std::min(1.f, aspect));
        factor = 12 * std::sin(halfAngle) / 1.1f;
    }
    scale = span > .001f ? std::clamp(factor / span, .05f, 8.f) : 1.f;
    cameraPan = {};
    update();
}
QMatrix4x4 Cube::matrix() const {
    QMatrix4x4 projection, view;
    float aspect = captureAspect > 0 ? captureAspect : float(width()) / float(std::max(1, height()));
    if (perspective) {
        projection.perspective(40.f, aspect, .05f, 200.f);
        view.translate(float(cameraPan.x()), float(cameraPan.y()), -6.f / scale);
    } else {
        float x = 2.2f * std::max(1.f, aspect) / scale,
              y = 2.2f * std::max(1.f, 1.f / aspect) / scale;
        projection.ortho(-x, x, -y, y, -100.f, 100.f);
        view.translate(float(cameraPan.x()), float(cameraPan.y()), 0);
    }
    view.rotate(pitch, 1, 0, 0);
    view.rotate(yaw, 0, 1, 0);
    const auto center = contextFraming ? QVector3D() : (scene().minimum + scene().maximum) / 2;
    view *= contextTransform;
    view.translate(-center);
    return projection * view;
}
QImage Cube::captureModel(bool comparison) {
    if (!isValid() || !modelMesh || modelMesh->faces.isEmpty() || !program || !program->isLinked())
        return {};
    makeCurrent();
    QImage pixels;
    {
        QOpenGLFramebufferObject framebuffer(256, 256, QOpenGLFramebufferObject::CombinedDepthStencil);
        if (framebuffer.isValid() && framebuffer.bind()) {
            GLint viewport[4];
            glGetIntegerv(GL_VIEWPORT, viewport);
            glViewport(0, 0, 256, 256);
            QScopedValueRollback aspect(captureAspect, 1.f);
            // Use the same block-centred camera and scale for both revisions.
            QScopedValueRollback fixed(contextFraming, comparison || contextFraming);
            QScopedValueRollback rotation(contextTransform, comparison ? QMatrix4x4{} : contextTransform);
            QScopedValueRollback pan(cameraPan, comparison ? QPointF{} : cameraPan);
            QScopedValueRollback viewPitch(pitch, comparison ? 25.f : pitch);
            QScopedValueRollback viewYaw(yaw, comparison ? 38.f : yaw);
            QScopedValueRollback zoom(scale, comparison ? .72f : scale);
            QScopedValueRollback projection(perspective, !comparison && perspective);
            QScopedValueRollback selection(selectedFace, comparison ? -1 : selectedFace);
            QScopedValueRollback guide(referenceFrame, comparison ? 0 : referenceFrame);
            paintGL();
            pixels = framebuffer.toImage();
            glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        }
    }
    doneCurrent();
    return pixels;
}
void Cube::uploadGeometry() {
    QVector<Vertex> vertices;
    vertices.reserve(scene().faces.size() * 6);
    for (const auto &f : scene().faces)
        for (int i : {0, 1, 2, 0, 2, 3}) {
            const auto p = f.points[i];
            const auto uv = f.uv[i];
            auto n = f.normal;
            if (!f.shadeDirection.isEmpty()) {
                const QStringList names{"down", "up", "north", "south", "west", "east"};
                const QVector3D ns[] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                        {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
                const auto k = names.indexOf(f.shadeDirection);
                if (k >= 0)
                    n = ns[k];
            }
            vertices << Vertex{
                p.x(), p.y(), p.z(), uv.x(), uv.y(), n.x(), n.y(), n.z(), f.shade ? 1.f : 0.f};
        }
    auto lineVertex = [](QVector3D p) { return Vertex{p.x(), p.y(), p.z(), 0, 0, 0, 0, 1, 0}; };
    QVector<Vertex> edges;
    edges.reserve(scene().faces.size() * 10);
    for (const auto &f : scene().faces)
        for (int i : {0, 1, 1, 2, 2, 3, 3, 0, 0, 2})
            edges << lineVertex(f.points[i]);
    lineBuffer.bind();
    lineBuffer.allocate(edges.constData(), int(edges.size() * sizeof(Vertex)));
    lineBuffer.release();
    QVector<Vertex> guides;
    for (int i = -4; i <= 4; ++i) {
        guides << lineVertex({float(i), -1, -4}) << lineVertex({float(i), -1, 4})
               << lineVertex({-4, -1, float(i)}) << lineVertex({4, -1, float(i)});
    }
    for (const auto &axis : {QVector3D(1.5, 0, 0), QVector3D(0, 1.5, 0), QVector3D(0, 0, 1.5)})
        guides << lineVertex({0, 0, 0}) << lineVertex(axis);
    // Context references (mesh units, one block = 2): untransformed block bounds, then
    // the 16 px GUI slot square in the view plane.
    for (int axis = 0; axis < 3; ++axis)
        for (int a : {-1, 1})
            for (int b : {-1, 1}) {
                QVector3D from, to;
                from[axis] = -1;
                to[axis] = 1;
                from[(axis + 1) % 3] = to[(axis + 1) % 3] = float(a);
                from[(axis + 2) % 3] = to[(axis + 2) % 3] = float(b);
                guides << lineVertex(from) << lineVertex(to);
            }
    const QVector3D slot[] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    for (int i = 0; i < 4; ++i)
        guides << lineVertex(slot[i]) << lineVertex(slot[(i + 1) % 4]);
    guideBuffer.bind();
    guideBuffer.allocate(guides.constData(), int(guides.size() * sizeof(Vertex)));
    guideBuffer.release();
    buffer.bind();
    buffer.allocate(vertices.constData(), int(vertices.size() * sizeof(Vertex)));
    buffer.release();
    vertexCount = int(vertices.size());
    geometryDirty = false;
    metrics->count("model_geometry_uploads", 1);
}
void Cube::initializeGL() {
    QElapsedTimer timer;
    timer.start();
    initializeOpenGLFunctions();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &Cube::cleanup,
            Qt::DirectConnection);
    metrics->environment["gpu"] =
        QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    metrics->environment["backend"] =
        QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_VERSION)));
    program = std::make_unique<QOpenGLShaderProgram>();
    bool ok = program->addShaderFromSourceCode(
        QOpenGLShader::Vertex,
        "attribute vec3 position; attribute vec2 texcoord; attribute vec3 normal; attribute float "
        "shade;"
        "uniform mat4 mvp; uniform vec3 direction; uniform float ambient; uniform float neutral;"
        "varying vec2 uv; varying float light; void "
        "main(){gl_Position=mvp*vec4(position,1.0);uv=texcoord;"
        "light=mix(1.0,ambient+(1.0-ambient)*max(dot(normal,normalize(direction)),0.0),shade*(1.0-"
        "neutral));}");
    ok = ok &&
         program->addShaderFromSourceCode(
             QOpenGLShader::Fragment,
             "uniform sampler2D tex; uniform float solid; uniform vec4 solidColor; uniform vec3 "
             "tintColor; varying vec2 uv; "
             "varying float light; void main(){if(solid>0.5){gl_FragColor=solidColor;return;}vec4 "
             "c=texture2D(tex,uv);"
             "if(c.a<0.001) discard; gl_FragColor=vec4(c.rgb*tintColor*light,c.a);}");
    if (!ok || !program->link() || !buffer.create() || !lineBuffer.create() ||
        !guideBuffer.create()) {
        setToolTip(program->log());
        return;
    }
    geometryDirty = materialsDirty = fullUpload = true;
    metrics->record("opengl_initialize", timer);
}
void Cube::paintGL() {
    QElapsedTimer timer;
    timer.start();
    glClearColor(19.f / 255, 22.f / 255, 30.f / 255, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if ((!document && !modelMesh) || !program || !program->isLinked() || !buffer.isCreated()) {
        QPainter p(this);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                   document ? "3D preview unavailable. Your texture remains editable."
                            : "Open a texture or Minecraft model");
        return;
    }
    if (geometryDirty)
        uploadGeometry();
    auto sampling = [this] {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    if (materialsDirty) {
        for (auto it = materialTextures.begin(); it != materialTextures.end();) {
            if (!it.key().isEmpty() && !materialImages.contains(it.key())) {
                auto t = it.value();
                glDeleteTextures(1, &t);
                materialSizes.remove(it.key());
                it = materialTextures.erase(it);
                metrics->count("material_texture_releases", 1);
            } else
                ++it;
        }
        QImage missing(2, 2, QImage::Format_RGBA8888);
        missing.fill(QColor(230, 0, 200));
        missing.setPixelColor(0, 0, Qt::black);
        missing.setPixelColor(1, 1, Qt::black);
        materialImages[QString()] = missing;
        for (auto i = materialImages.cbegin(); i != materialImages.cend(); ++i)
            dirtyMaterials.insert(i.key());
        materialsDirty = false;
    }
    if (!dirtyMaterials.isEmpty()) {
        const auto pending = dirtyMaterials;
        dirtyMaterials.clear();
        for (const auto &key : pending) {
            auto it = materialImages.constFind(key);
            if (it == materialImages.cend())
                continue;
            if (it->isNull())
                continue;
            GLuint t;
            t = materialTextures.value(it.key());
            if (!t) {
                glGenTextures(1, &t);
                metrics->count("material_texture_allocations", 1);
            }
            glBindTexture(GL_TEXTURE_2D, t);
            sampling();
            const auto image = it->convertToFormat(QImage::Format_RGBA8888);
            if (materialSizes.value(it.key()) != image.size()) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, image.constBits());
                materialSizes[it.key()] = image.size();
            } else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width(), image.height(), GL_RGBA,
                                GL_UNSIGNED_BYTE, image.constBits());
            metrics->count("material_texture_uploads", 1);
            materialTextures[it.key()] = t;
        }
    }
    if (document) {
        if (!texture) {
            glGenTextures(1, &texture);
            metrics->count("texture_allocations", 1);
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        sampling();
        const auto &displayed = playbackPreview.isNull() ? document->image : playbackPreview;
        if (fullUpload || textureSize != displayed.size()) {
            const bool allocate = textureSize != displayed.size();
            textureSize = displayed.size();
            if (allocate)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureSize.width(), textureSize.height(),
                             0, GL_RGBA, GL_UNSIGNED_BYTE, displayed.constBits());
            else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureSize.width(), textureSize.height(),
                                GL_RGBA, GL_UNSIGNED_BYTE, displayed.constBits());
            metrics->count("full_uploads", 1);
            metrics->count("upload_bytes", displayed.sizeInBytes());
            fullUpload = false;
            document->dirty = {};
        } else if (playbackPreview.isNull() && !document->dirty.isEmpty()) {
            auto r = document->dirty.intersected(document->image.rect());
            auto patch = document->image.copy(r);
            glTexSubImage2D(GL_TEXTURE_2D, 0, r.x(), r.y(), r.width(), r.height(), GL_RGBA,
                            GL_UNSIGNED_BYTE, patch.constBits());
            metrics->count("partial_uploads", 1);
            metrics->count("upload_bytes", patch.sizeInBytes());
            document->dirty = {};
        }
    }
    auto m = matrix();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    program->bind();
    program->setUniformValue("mvp", m);
    program->setUniformValue("tex", 0);
    program->setUniformValue("solid", 0.f);
    program->setUniformValue("direction", lightDirection);
    program->setUniformValue("ambient", std::clamp(ambient, 0.f, 1.f));
    program->setUniformValue("neutral", neutral ? 1.f : 0.f);
    buffer.bind();
    auto attribute = [&](const char *name, int offset, int count) {
        int a = program->attributeLocation(name);
        program->enableAttributeArray(a);
        program->setAttributeBuffer(a, GL_FLOAT, offset * sizeof(float), count, sizeof(Vertex));
    };
    attribute("position", 0, 3);
    attribute("texcoord", 3, 2);
    attribute("normal", 5, 3);
    attribute("shade", 8, 1);
    QVector<int> order(scene().faces.size());
    std::iota(order.begin(), order.end(), 0);
    auto imageFor = [&](const mc::Face &f) -> QImage {
        return document && (!modelMesh || f.texture == activeTexture) && !materialPreview
                   ? (playbackPreview.isNull() ? document->image : playbackPreview)
                   : materialImages.value(f.texture);
    };
    // Classify each material once per paint, not once per face. Pixel uploads and meshes stay
    // independent.
    QHash<QString, bool> translucent;
    for (const auto &f : scene().faces) {
        if (!translucent.contains(f.texture)) {
            const auto image = imageFor(f);
            auto info = alphaCache.value(f.texture);
            if (info.key != image.cacheKey()) {
                info = {image.cacheKey(), false};
                if (image.hasAlphaChannel())
                    for (int y = 0; y < image.height() && !info.partial; ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            int a = qAlpha(image.pixel(x, y));
                            if (a > 0 && a < 255) {
                                info.partial = true;
                                break;
                            }
                        }
                alphaCache[f.texture] = info;
                metrics->count("material_alpha_scans", 1);
            }
            translucent[f.texture] = info.partial;
        }
        if (f.forceTranslucent)
            translucent[f.texture] = true;
    }
    auto depth = [&](int i) {
        auto c = (scene().faces[i].points[0] + scene().faces[i].points[2]) / 2;
        return m.map(c).z();
    };
    // Generated layers share their front/back planes. Keep all of them in the blended
    // pass: splitting opaque upper layers from translucent lower layers reverses their
    // authored order. Stable depth sorting preserves layer order on coincident planes.
    auto blended = [&](int i) {
        const auto &face = scene().faces[i];
        return face.generatedLayer >= 0 || translucent[face.texture];
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        bool ta = blended(a), tb = blended(b);
        return ta != tb ? !ta : depth(a) > depth(b);
    });
    for (int i : order) {
        const auto &f = scene().faces[i];
        program->setUniformValue(
            "tintColor",
            QVector3D(qRed(f.color) / 255.f, qGreen(f.color) / 255.f, qBlue(f.color) / 255.f));
        GLuint t = document && (!modelMesh || f.texture == activeTexture) && !materialPreview
                       ? texture
                       : materialTextures.value(f.texture, materialTextures.value(QString()));
        glBindTexture(GL_TEXTURE_2D, t);
        glDepthMask(blended(i) ? GL_FALSE : GL_TRUE);
        glDrawArrays(GL_TRIANGLES, i * 6, 6);
    }
    glDepthMask(GL_TRUE);
    buffer.release();
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    // Reusable line buffers keep outlines, wireframe and guides in the same GL
    // pipeline. Qt vector overlays were suppressed on the verified Radeon path.
    program->setUniformValue("solid", 1.f);
    auto bindLines = [&](QOpenGLBuffer &lines) {
        lines.bind();
        attribute("position", 0, 3);
        attribute("texcoord", 3, 2);
        attribute("normal", 5, 3);
        attribute("shade", 8, 1);
    };
    bindLines(lineBuffer);
    for (int i = 0; i < scene().faces.size(); ++i)
        if (wireframe || outlines || i == selectedFace) {
            program->setUniformValue("solidColor", i == selectedFace
                                                       ? QVector4D(1, .82f, .31f, 1)
                                                       : QVector4D(.76f, .8f, .94f, .6f));
            glLineWidth(i == selectedFace ? 2.f : 1.f);
            glDrawArrays(GL_LINES, i * 10, wireframe ? 10 : 8);
        }
    lineBuffer.release();
    bindLines(guideBuffer);
    if (groundGrid) {
        program->setUniformValue("solidColor", QVector4D(.32f, .34f, .4f, .4f));
        glLineWidth(1);
        glDrawArrays(GL_LINES, 0, 36);
    }
    if (axes) {
        const QVector4D colors[] = {{1, .2f, .2f, 1}, {.2f, 1, .2f, 1}, {.2f, .8f, 1, 1}};
        for (int i = 0; i < 3; ++i) {
            program->setUniformValue("solidColor", colors[i]);
            glLineWidth(2);
            glDrawArrays(GL_LINES, 36 + i * 2, 2);
        }
    }
    if (referenceFrame) {
        program->setUniformValue("solidColor", QVector4D(.95f, .62f, .3f, .7f));
        glLineWidth(1);
        glDrawArrays(GL_LINES, referenceFrame == 2 ? 66 : 42, referenceFrame == 2 ? 8 : 24);
    }
    guideBuffer.release();
    glLineWidth(1);
    program->release();
    // QPainter overlays below share this context: leave no model vertex arrays enabled,
    // or its fills read them and are dropped.
    for (auto name : {"position", "texcoord", "normal", "shade"})
        program->disableAttributeArray(name);
    if (modelMesh && scene().faces.isEmpty()) {
        QPainter p(this);
        p.setPen(Qt::lightGray);
        p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                   "No supported geometry. See Models / UV diagnostics.");
    }
    metrics->record("preview_submit_cpu", timer);
}
int Cube::faceAt(QPointF position) const {
    bool ok = false;
    const auto inv = matrix().inverted(&ok);
    if (!ok)
        return -1;
    float x = float(position.x() / std::max(1, width()) * 2 - 1),
          y = float(1 - position.y() / std::max(1, height()) * 2);
    auto origin = inv.map(QVector3D(x, y, -1));
    auto ray = (inv.map(QVector3D(x, y, 1)) - origin).normalized();
    float closest = 1e30f;
    int selected = -1;
    for (int i = 0; i < scene().faces.size(); ++i)
        for (const auto &tri : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
            const auto &f = scene().faces[i];
            auto a = f.points[tri[0]], e1 = f.points[tri[1]] - a, e2 = f.points[tri[2]] - a;
            auto h = QVector3D::crossProduct(ray, e2);
            float det = QVector3D::dotProduct(e1, h);
            if (det < 1e-7f)
                continue;
            auto s = origin - a;
            float u = QVector3D::dotProduct(s, h) / det;
            if (u < 0 || u > 1)
                continue;
            auto q = QVector3D::crossProduct(s, e1);
            float v = QVector3D::dotProduct(ray, q) / det;
            if (v < 0 || u + v > 1)
                continue;
            float t = QVector3D::dotProduct(e2, q) / det;
            if (t >= 0 && t < closest) {
                closest = t;
                selected = i;
            }
        }
    return selected;
}
void Cube::mousePressEvent(QMouseEvent *e) { last = pressed = e->position(); }
void Cube::mouseMoveEvent(QMouseEvent *e) {
    auto d = e->position() - last;
    const auto keys = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (displayManipulation && (e->buttons() & Qt::LeftButton) && keys) {
        // Shift: rotate display, Ctrl: translate, Ctrl+Shift: uniform scale.
        last = e->position();
        emit displayDragged(d, keys);
        return;
    }
    if (e->buttons() & (Qt::MiddleButton | Qt::RightButton))
        cameraPan += QPointF(d.x(), -d.y()) * .008 / scale;
    else if (e->buttons() & Qt::LeftButton) {
        pitch += float(d.y()) * .57f;
        yaw += float(d.x()) * .57f;
    }
    last = e->position();
    if (e->buttons())
        update();
}
void Cube::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && (e->position() - pressed).manhattanLength() < 4) {
        selectedFace = faceAt(e->position());
        update();
        emit faceSelected(selectedFace);
    }
}
void Cube::wheelEvent(QWheelEvent *e) {
    scale = float(std::clamp(double(scale) * std::pow(1.002, e->angleDelta().y() / 3.), .05, 8.));
    update();
}
} // namespace nova
