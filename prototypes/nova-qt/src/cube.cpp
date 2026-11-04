#include "cube.h"
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QWheelEvent>
namespace nova {
Cube::Cube(Metrics *m, QWidget *p) : QOpenGLWidget(p), metrics(m) {
    setMinimumSize(200, 150);
    setFocusPolicy(Qt::StrongFocus);
}
Cube::~Cube() {
    cleanup();
    if (context())
        disconnect(context(), nullptr, this, nullptr);
}
void Cube::cleanup() {
    if (context()) {
        makeCurrent();
        buffer.destroy();
        if (texture)
            glDeleteTextures(1, &texture);
        program.reset();
        texture = 0;
        textureSize = {};
        fullUpload = true;
        doneCurrent();
    }
}
void Cube::setDocument(std::shared_ptr<Document> doc) {
    if (doc != document) {
        document = std::move(doc);
        fullUpload = true;
    }
    update();
}
void Cube::initializeGL() {
    initializeOpenGLFunctions();
    program = std::make_unique<QOpenGLShaderProgram>();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &Cube::cleanup,
            Qt::DirectConnection);
    metrics->environment["gpu"] =
        QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    metrics->environment["backend"] =
        QString::fromLatin1(reinterpret_cast<const char *>(glGetString(GL_VERSION)));
    bool ok = program->addShaderFromSourceCode(
        QOpenGLShader::Vertex,
        "attribute vec3 position; attribute vec2 texcoord; attribute float "
        "shade; uniform mat4 mvp; varying vec2 uv; varying float light; void "
        "main(){gl_Position=mvp*vec4(position,1.0);uv=texcoord;light=shade;}");
    ok = ok && program->addShaderFromSourceCode(
                   QOpenGLShader::Fragment,
                   "uniform sampler2D tex; varying vec2 uv; varying float light; "
                   "void main(){vec4 "
                   "c=texture2D(tex,uv);gl_FragColor=vec4(c.rgb*light,c.a);}");
    if (!ok || !program->link()) {
        setToolTip(program->log());
        return;
    }
    buffer.create();
    glGenTextures(1, &texture);
}
void Cube::paintGL() {
    QElapsedTimer timer;
    timer.start();
    glClearColor(19.f / 255, 22.f / 255, 30.f / 255, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!document || !program || !program->isLinked())
        return;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (fullUpload || textureSize != document->image.size()) {
        textureSize = document->image.size();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureSize.width(), textureSize.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, document->image.constBits());
        metrics->count("full_uploads", 1);
        metrics->count("upload_bytes", document->image.sizeInBytes());
        fullUpload = false;
        document->dirty = {};
    } else if (!document->dirty.isEmpty()) {
        auto r = document->dirty;
        auto patch = document->image.copy(r);
        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x(), r.y(), r.width(), r.height(), GL_RGBA,
                        GL_UNSIGNED_BYTE, patch.constBits());
        metrics->count("partial_uploads", 1);
        metrics->count("upload_bytes", patch.sizeInBytes());
        document->dirty = {};
    }
    struct Vertex {
        float x, y, z, u, v, shade;
    };
    const float points[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3},
                             {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}};
    const float shades[6] = {.72f, .9f, .65f, .8f, 1, .55f};
    const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    QVector<Vertex> vertices;
    for (int f = 0; f < 6; ++f)
        for (int i : {0, 1, 2, 0, 2, 3}) {
            auto p = points[faces[f][i]];
            vertices.append({p[0], p[1], p[2], uv[i][0], uv[i][1], shades[f]});
        }
    QMatrix4x4 m;
    float aspect = float(width()) / float(std::max(1, height()));
    float x = 2.2f * std::max(1.f, aspect) / scale, y = 2.2f * std::max(1.f, 1.f / aspect) / scale;
    m.ortho(-x, x, -y, y, -10, 10);
    m.rotate(pitch, 1, 0, 0);
    m.rotate(yaw, 0, 1, 0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    program->bind();
    program->setUniformValue("mvp", m);
    program->setUniformValue("tex", 0);
    buffer.bind();
    buffer.allocate(vertices.constData(), int(vertices.size() * sizeof(Vertex)));
    int pos = program->attributeLocation("position"), tex = program->attributeLocation("texcoord"),
        shade = program->attributeLocation("shade");
    program->enableAttributeArray(pos);
    program->setAttributeBuffer(pos, GL_FLOAT, 0, 3, sizeof(Vertex));
    program->enableAttributeArray(tex);
    program->setAttributeBuffer(tex, GL_FLOAT, 3 * sizeof(float), 2, sizeof(Vertex));
    program->enableAttributeArray(shade);
    program->setAttributeBuffer(shade, GL_FLOAT, 5 * sizeof(float), 1, sizeof(Vertex));
    glDrawArrays(GL_TRIANGLES, 0, int(vertices.size()));
    buffer.release();
    program->release();
    glDisable(GL_DEPTH_TEST);
    metrics->record("preview_submit_cpu", timer);
}
void Cube::mousePressEvent(QMouseEvent *e) { last = e->position(); }
void Cube::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() & Qt::LeftButton) {
        auto d = e->position() - last;
        pitch += float(d.y()) * .57f;
        yaw += float(d.x()) * .57f;
        update();
    }
    last = e->position();
}
void Cube::wheelEvent(QWheelEvent *e) {
    scale = float(std::clamp(double(scale) * std::pow(1.002, e->angleDelta().y() / 3.0), .3, 2.0));
    update();
}
} // namespace nova
