// SPDX-License-Identifier: GPL-3.0-or-later
#include "metrics.h"
#include "core.h"
#include "index.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
namespace nova {
void Metrics::record(const QString &name, const QElapsedTimer &timer) {
    auto &list = samples[name];
    if (list.size() < 20000)
        list.append(timer.nsecsElapsed() / 1e6);
}
void Metrics::count(const QString &name, qint64 value) {
    counters[name] = counters.value(name).toDouble() + double(value);
}
bool Metrics::write(const QString &path) const {
    QJsonObject s;
    for (auto it = samples.begin(); it != samples.end(); ++it)
        s[it.key()] = it.value();
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(QJsonDocument(QJsonObject{{"samples_ms", s},
                                                {"counters", counters},
                                                {"environment", environment}})
                          .toJson()) >= 0;
}
bool benchmark(const QString &root, const QString &output) {
    Metrics m;
    Index index;
    QElapsedTimer t;
    for (int i = 0; i < 5; ++i) {
        t.start();
        index = Index::scan(root);
        m.record("index", t);
        if (!index.error.isEmpty())
            return false;
    }
    QStringList pngFiles;
    for (auto path : index.files)
        if (path.endsWith(".png", Qt::CaseInsensitive))
            pngFiles.append(path);
    m.count("assets", pngFiles.size());
    m.count("indexed_files", index.files.size());
    for (auto p : pngFiles.mid(0, 200)) {
        t.start();
        auto im = loadPng(root + '/' + p);
        if (im.isNull())
            return false;
        auto thumb = im.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        if (thumb.isNull())
            return false;
        m.record("thumbnail_decode_resize", t);
    }
    QTemporaryDir tmp;
    QImage im(256, 256, QImage::Format_RGBA8888);
    im.fill(Qt::transparent);
    Document doc(tmp.path() + "/save.png", im);
    for (int i = 0; i < 1000; ++i) {
        QPoint p(i % 256, (i / 256) % 256);
        t.start();
        doc.draw(p, p, qRgba(i % 256, 72, 180, 255));
        doc.finish();
        m.record("brush_command", t);
        t.start();
        doc.undo();
        m.record("undo", t);
        t.start();
        doc.redo();
        m.record("redo", t);
    }
    for (int i = 0; i < 30; ++i) {
        t.start();
        if (!doc.save())
            return false;
        m.record("save_256", t);
    }
    m.count("retained_history_bytes", doc.historyBytes());
    return m.write(output);
}
} // namespace nova
