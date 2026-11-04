// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class SessionTests : public QObject {
    Q_OBJECT
    QImage image() {
        QImage im(16, 16, QImage::Format_RGBA8888);
        im.fill(Qt::transparent);
        return im;
    }
  private slots:
    void recoveryPixelBudget() {
        nova::DocumentSession session;
        QImage large(4096, 4096, QImage::Format_RGBA8888);
        large.fill(0);
        for (int i = 0; i < 7; ++i)
            QVERIFY(session.adopt(
                std::make_shared<nova::Document>(QString("missing-%1.png").arg(i), large)));
        auto small = std::make_shared<nova::Document>("small.png", image());
        QVERIFY(session.adopt(small));
        QImage medium(4096, 2048, QImage::Format_RGBA8888);
        medium.fill(0);
        QVERIFY(session.adopt(std::make_shared<nova::Document>("medium.png", medium)));
        QVERIFY(!session.canRestorePixels(small, large));
        QVERIFY(session.canRestorePixels(small, image()));
    }
    void lifecycleAndCommands() {
        QTemporaryDir tmp;
        auto path = tmp.path() + "/texture.png";
        QVERIFY(nova::savePng(path, image()));
        nova::DocumentSession session;
        QSignalSpy changed(&session, &nova::DocumentSession::changed);
        auto doc = session.open(path);
        QVERIFY(doc);
        auto id = doc->id();
        QCOMPARE(doc->state(), nova::Document::State::Ready);
        QCOMPARE(session.open(tmp.path() + "/./texture.png"), doc);
        QCOMPARE(session.count(), 1);
        doc->draw({1, 1}, {5, 1}, qRgba(7, 8, 9, 111));
        doc->finish();
        QCOMPARE(doc->state(), nova::Document::State::Modified);
        QVERIFY(session.dispatch(doc, nova::Command::Undo));
        QVERIFY(!doc->modified());
        QVERIFY(session.dispatch(doc, nova::Command::Redo));
        QVERIFY(doc->modified());
        QVERIFY(session.dispatch(doc, nova::Command::Save));
        QVERIFY(!doc->modified());
        auto target = tmp.path() + "/copy.png";
        QVERIFY(session.dispatch(doc, nova::Command::SaveAs, target));
        QCOMPARE(doc->path, target);
        QCOMPARE(doc->id(), id);
        QCOMPARE(nova::loadPng(target), doc->image);
        QCOMPARE(changed.count(), 4);
        session.close(doc);
        QCOMPARE(session.count(), 0);
        QCOMPARE(doc->state(), nova::Document::State::Closing);
        QVERIFY(!session.dispatch(doc, nova::Command::Undo));
    }
    void failuresPreserveOriginal() {
        QTemporaryDir tmp;
        auto path = tmp.path() + "/original.png";
        auto original = image();
        QVERIFY(nova::savePng(path, original));
        nova::DocumentSession session;
        auto doc = session.open(path);
        QVERIFY(doc);
        doc->draw({1, 1}, {1, 1}, qRgba(1, 2, 3, 4));
        doc->finish();
        QString error;
        QVERIFY(!session.dispatch(doc, nova::Command::SaveAs, tmp.path() + "/missing/file.png",
                                  false, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(doc->state(), nova::Document::State::Error);
        QVERIFY(doc->modified());
        QCOMPARE(doc->path, path);
        QCOMPARE(nova::loadPng(path), original);
        auto other = tmp.path() + "/other.png";
        QVERIFY(nova::savePng(other, original));
        QVERIFY(!doc->saveAs(other, false, &error));
        QCOMPARE(nova::loadPng(other), original);
        auto second = session.open(other);
        QVERIFY(second);
        QVERIFY(!session.dispatch(doc, nova::Command::SaveAs, other, true, &error));
        QVERIFY(session.dispatch(doc, nova::Command::Save));
        QCOMPARE(doc->state(), nova::Document::State::Ready);
        QVERIFY(!session.open(tmp.path() + "/missing.png", &error));
    }
    void noOpStrokeKeepsRedo() {
        nova::Document doc({}, image());
        doc.draw({1, 1}, {1, 1}, qRgba(1, 2, 3, 4));
        doc.finish();
        doc.undo();
        doc.draw({2, 2}, {2, 2}, qRgba(9, 8, 7, 6));
        doc.draw({2, 2}, {2, 2}, 0);
        doc.finish();
        QVERIFY(!doc.modified());
        QVERIFY(doc.canRedo());
        doc.redo();
        QCOMPARE(doc.image.pixel(1, 1), qRgba(1, 2, 3, 4));
    }
    void malformedAndDimensions() {
        QTemporaryDir tmp;
        QFile file(tmp.path() + "/bad.png");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a png");
        file.close();
        QString error;
        QVERIFY(!nova::Document::open(file.fileName(), &error));
        QVERIFY(!error.isEmpty());
        QImage wide(4097, 1, QImage::Format_RGBA8888);
        wide.fill(0);
        // Use Qt directly to create a valid but unsupported dimension fixture.
        auto path = tmp.path() + "/wide.png";
        QVERIFY(wide.save(path));
        QVERIFY(nova::loadPng(path, &error).isNull());
        QVERIFY(!nova::savePng(path, QImage{}, &error));
        QVERIFY(QFileInfo(path).size() > 0);
    }
};
QTEST_GUILESS_MAIN(SessionTests)
#include "session_test.moc"
