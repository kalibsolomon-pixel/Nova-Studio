// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback.h"
#include "minecraft_animation.h"
#include <QTest>
#include <QSignalSpy>
using namespace nova;
class AnimationTimingTest : public QObject {
    Q_OBJECT
    static std::shared_ptr<Document> document() {
        QImage image(16, 16, QImage::Format_RGBA8888); image.fill(Qt::red);
        auto doc = std::make_shared<Document>("test.png", image); doc->convertToAnimation(); doc->editFrame(FrameCommand::Duplicate);
        auto state = doc->animationState(); state.defaultTicks = 2;
        state.entries = {{state.frames[0].id, 0}, {state.frames[1].id, 8}, {state.frames[0].id, 1}};
        doc->replaceAnimation(state, "timing"); return doc;
    }
  private slots:
    void deterministicBoundariesAndLongRun() {
        auto doc = document(); AnimationSequence sequence; QVERIFY(sequence.set(doc->animationState()));
        QCOMPARE(sequence.ticks(), 11);
        QCOMPARE(sequence.at(0).entry, 0); QCOMPARE(sequence.at(99.99).entry, 0);
        QCOMPARE(sequence.at(100).entry, 1); QCOMPARE(sequence.at(499.99).entry, 1);
        QCOMPARE(sequence.at(500).entry, 2); QCOMPARE(sequence.at(550).entry, 0);
        QCOMPARE(sequence.at(550000000000.0 + 100).entry, 1);
        QVERIFY(sequence.at(550, false).ended); QCOMPARE(sequence.at(550, false).entry, 2);
        QCOMPARE(sequence.at(150).fraction, .125);
        auto invalid = doc->animationState(); invalid.entries[0].ticks = -1; QVERIFY(!sequence.set(invalid));
    }
    void pauseResumeSeekSpeedIdle() {
        auto doc = document(); PlaybackController controller; controller.setDocument(doc);
        QSignalSpy frames(&controller, &PlaybackController::displayed);
        QVERIFY(!controller.timerActive()); controller.play(); QVERIFY(controller.timerActive());
        QTest::qWait(70); controller.pause(); QVERIFY(!controller.timerActive());
        double position = controller.positionMilliseconds(); QTest::qWait(60); QCOMPARE(controller.positionMilliseconds(), position);
        controller.seek(1); QCOMPARE(controller.positionMilliseconds(), 100.0);
        controller.setSpeed(2); controller.play(); QTest::qWait(60); controller.pause();
        QVERIFY(controller.positionMilliseconds() >= 200); QVERIFY(controller.positionMilliseconds() < 500);
        QVERIFY(frames.count() >= 2); controller.stop(); QVERIFY(!controller.previewing()); QVERIFY(!controller.timerActive());
        QCOMPARE(controller.positionMilliseconds(), 0.0);
        QCOMPARE(doc->image.pixelColor(0, 0), QColor(Qt::red));
    }
    void interpolationBlendsRgbaWithoutEditingSources() {
        QImage a(1, 1, QImage::Format_RGBA8888), b(a.size(), a.format());
        a.setPixelColor(0, 0, QColor(200, 0, 0, 50)); b.setPixelColor(0, 0, QColor(0, 100, 200, 240));
        auto image = interpolateFrames(a, b, .5); QCOMPARE(image.pixelColor(0, 0), QColor(100, 50, 100, 145));
        QCOMPARE(a.pixelColor(0, 0), QColor(200, 0, 0, 50));
    }
};
QTEST_GUILESS_MAIN(AnimationTimingTest)
#include "animation_timing_test.moc"
