// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback.h"
#include "minecraft_animation.h"
#include <algorithm>
#include <cmath>
namespace nova {
bool AnimationSequence::set(const AnimationState &state) {
    boundaries.clear();
    if (state.defaultTicks < 1 || state.entries.empty()) return false;
    boundaries.push_back(0);
    for (const auto &entry : state.entries) {
        if (entry.ticks < 0 || std::none_of(state.frames.begin(), state.frames.end(),
                                           [&](const auto &frame) { return frame.id == entry.frame; })) {
            boundaries.clear(); return false;
        }
        const auto ticks = entry.ticks ? entry.ticks : state.defaultTicks;
        if (boundaries.back() > std::numeric_limits<qint64>::max() - ticks) { boundaries.clear(); return false; }
        boundaries.push_back(boundaries.back() + ticks);
    }
    return true;
}
PlaybackPosition AnimationSequence::at(double milliseconds, bool loop) const {
    if (boundaries.size() < 2 || !std::isfinite(milliseconds)) return {};
    double ticks = std::max(0.0, milliseconds / 50.0);
    const double total = double(boundaries.back());
    bool ended = !loop && ticks >= total;
    ticks = loop ? std::fmod(ticks, total) : std::min(ticks, std::nextafter(total, 0.0));
    const auto it = std::upper_bound(boundaries.begin(), boundaries.end(), qint64(std::floor(ticks)));
    const int index = std::clamp(int(it - boundaries.begin()) - 1, 0, int(boundaries.size()) - 2);
    const int next = index + 1 < int(boundaries.size()) - 1 ? index + 1 : loop ? 0 : index;
    // Admitted versions encode game-tick progress at 1/1000 precision in the sprite shader.
    const double fraction = std::floor(1000 * std::floor(ticks - double(boundaries[size_t(index)])) /
                            double(boundaries[size_t(index) + 1] - boundaries[size_t(index)])) / 1000;
    return {index, next, fraction, ended};
}
double AnimationSequence::entryMilliseconds(int index) const {
    return index >= 0 && index + 1 < int(boundaries.size()) ? double(boundaries[size_t(index)]) * 50 : 0;
}
double AnimationSequence::nextChangeMilliseconds(double milliseconds, bool interpolate) const {
    if (boundaries.size() < 2 || !std::isfinite(milliseconds))
        return 1000;
    const double ticks = std::max(0.0, milliseconds / 50.0);
    if (interpolate)
        return (std::floor(ticks) + 1 - ticks) * 50;
    const double local = std::fmod(ticks, double(boundaries.back()));
    const auto next =
        std::upper_bound(boundaries.begin(), boundaries.end(), qint64(std::floor(local)));
    return (double(*next) - local) * 50;
}
PlaybackController::PlaybackController(QObject *parent) : QObject(parent) {
    timer.setObjectName("texturePlaybackDeadline");
    timer.setSingleShot(true);
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, &PlaybackController::present);
}
void PlaybackController::setDocument(std::shared_ptr<Document> doc) {
    if (doc == document && (!doc || serial == doc->changeSerial())) return;
    stop();
    document = std::move(doc);
    state = {};
    if (document && document->animated()) {
        state = document->animationState();
        // Playback retains references and timing only, avoiding image ownership/copies.
        for (auto &frame : state.frames) frame.layers.clear();
        sequence.set(state);
        serial = document->changeSerial();
    }
}
double PlaybackController::positionMilliseconds() const {
    return offset + (running ? double(elapsed.nsecsElapsed()) / 1000000.0 * multiplier : 0);
}
void PlaybackController::play() {
    if (running || !document || !document->animated() || !sequence.ticks()) return;
    showing = running = true;
    elapsed.start();
    present();
    emit statusChanged();
}
void PlaybackController::pause() {
    if (!running) return;
    offset = positionMilliseconds(); running = false; timer.stop(); emit statusChanged();
}
void PlaybackController::stop() {
    running = showing = false; timer.stop(); offset = 0; lastEntry = lastTick = -1;
    emit stopped(); emit statusChanged();
}
void PlaybackController::seek(int entry) {
    offset = sequence.entryMilliseconds(entry);
    if (running) elapsed.restart();
    lastEntry = lastTick = -1;
    if (showing) present();
}
void PlaybackController::setSpeed(double value) {
    if (!std::isfinite(value) || value < .1 || value > 8) return;
    offset = positionMilliseconds(); multiplier = value;
    if (running) {
        elapsed.restart();
        present();
    }
}
void PlaybackController::present() {
    if (!document || !showing) return;
    if (serial != document->changeSerial()) { setDocument(document); return; }
    auto position = sequence.at(positionMilliseconds(), looping);
    if (position.entry < 0) { stop(); return; }
    const int tick = int(position.fraction * 1000000);
    if (position.entry != lastEntry || (state.interpolate && tick != lastTick)) {
        auto pixels = document->frameComposite(state.entries[size_t(position.entry)].frame);
        if (state.interpolate && position.fraction > 0)
            pixels = interpolateFrames(pixels, document->frameComposite(state.entries[size_t(position.next)].frame), position.fraction);
        emit displayed(pixels, position.entry);
        lastEntry = position.entry; lastTick = tick;
    }
    if (position.ended) { pause(); }
    if (running)
        timer.start(std::max(1, int(std::ceil(sequence.nextChangeMilliseconds(
                                                  positionMilliseconds(), state.interpolate) /
                                              multiplier))));
}
} // namespace nova
