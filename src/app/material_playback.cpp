// SPDX-License-Identifier: GPL-3.0-or-later
#include "material_playback.h"
#include "minecraft_animation.h"
#include <cmath>
namespace nova {
MaterialPlayback::MaterialPlayback(QObject *parent) : QObject(parent) {
    timer.setSingleShot(true);
    timer.setTimerType(Qt::PreciseTimer);
    timer.setObjectName("materialPlaybackDeadline");
    connect(&timer, &QTimer::timeout, this, &MaterialPlayback::present);
}
double MaterialPlayback::positionMilliseconds() const {
    return offset + (running ? elapsed.nsecsElapsed() / 1e6 : 0);
}
void MaterialPlayback::synchronize(Material &m) {
    if (!m.document || m.serial == m.document->changeSerial())
        return;
    m.serial = m.document->changeSerial();
    m.state = m.document->animationState();
    for (auto &f : m.state.frames)
        f.layers.clear();
    m.sequence.set(m.state);
    m.entry = m.tick = -1;
}
void MaterialPlayback::setMaterials(const QMap<QString, std::shared_ptr<Document>> &documents) {
    const bool changed = materials.keys() != documents.keys();
    if (changed)
        stop();
    auto previous = std::move(materials);
    materials.clear();
    for (auto i = documents.cbegin(); i != documents.cend(); ++i) {
        if (!i.value() || !i.value()->animated())
            continue;
        auto value = previous.value(i.key());
        if (value.document != i.value()) {
            value.document = i.value();
            value.serial = 0;
        }
        synchronize(value);
        materials[i.key()] = std::move(value);
    }
    if (materials.isEmpty())
        stop();
    else if (visible)
        present();
    emit statusChanged();
}
void MaterialPlayback::updateDocument(const QString &asset, std::shared_ptr<Document> document) {
    auto i = materials.find(asset);
    if (i == materials.end()) {
        if (!document || !document->animated())
            return;
        Material m;
        m.document = document;
        synchronize(m);
        materials.insert(asset, std::move(m));
        i = materials.find(asset);
        emit statusChanged();
    }
    if (!document || !document->animated()) {
        materials.erase(i);
        if (materials.isEmpty())
            stop();
        return;
    }
    if (i->document != document) {
        i->document = std::move(document);
        i->serial = 0;
    }
    synchronize(*i);
    if (visible)
        present();
}
void MaterialPlayback::setVisible(bool value) {
    if (visible == value)
        return;
    offset = positionMilliseconds();
    visible = value;
    running = requested && visible && !materials.isEmpty();
    timer.stop();
    if (running)
        elapsed.restart();
    if (visible)
        present();
}
void MaterialPlayback::play() {
    if (requested || materials.isEmpty())
        return;
    requested = true;
    running = visible;
    elapsed.restart();
    if (running)
        present();
    emit statusChanged();
}
void MaterialPlayback::pause() {
    offset = positionMilliseconds();
    requested = running = false;
    timer.stop();
    emit statusChanged();
}
void MaterialPlayback::stop() {
    requested = running = false;
    timer.stop();
    offset = 0;
    for (auto &m : materials)
        m.entry = m.tick = -1;
    emit statusChanged();
}
void MaterialPlayback::seek(double ms) {
    if (!std::isfinite(ms) || ms < 0)
        return;
    offset = ms;
    if (running)
        elapsed.restart();
    for (auto &m : materials)
        m.entry = m.tick = -1;
    if (visible)
        present();
}
void MaterialPlayback::present() {
    if (!visible) {
        timer.stop();
        return;
    }
    const double ms = positionMilliseconds();
    double delay = 60000;
    for (auto i = materials.begin(); i != materials.end(); ++i) {
        auto &m = i.value();
        synchronize(m);
        auto p = m.sequence.at(ms);
        if (p.entry < 0)
            continue;
        const int tick = int(p.fraction * 1000000);
        if (p.entry != m.entry || (m.state.interpolate && tick != m.tick)) {
            auto image = m.document->frameComposite(m.state.entries[size_t(p.entry)].frame);
            if (m.state.interpolate && p.fraction > 0)
                image = interpolateFrames(
                    image, m.document->frameComposite(m.state.entries[size_t(p.next)].frame),
                    p.fraction);
            m.image = image;
            m.entry = p.entry;
            m.tick = tick;
            emit frame(i.key(), image);
        }
        delay = std::min(delay, m.sequence.nextChangeMilliseconds(ms, m.state.interpolate));
    }
    if (running)
        timer.start(std::max(1, int(std::ceil(delay))));
}
} // namespace nova
