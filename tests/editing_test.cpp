// SPDX-License-Identifier: GPL-3.0-or-later
// Document-level editing tests for Phase 2-C3: selections, strokes, fills,
// shapes, raster/document operations, floating commits, history and storage.
#include "recovery.h"
#include "session.h"
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

using namespace nova;
namespace {
QImage blank(int w = 16, int h = 16) {
    QImage im(w, h, QImage::Format_RGBA8888);
    im.fill(Qt::transparent);
    return im;
}
std::shared_ptr<Document> doc(int w = 16, int h = 16) {
    return std::make_shared<Document>(QString(), blank(w, h));
}
QRgb at(const std::shared_ptr<Document> &d, int x, int y, int layer = -1) {
    return layer < 0 ? d->image.pixel(x, y) : d->layers()[size_t(layer)].pixels.pixel(x, y);
}
StrokeStyle style(QRgb color = qRgb(255, 0, 0), int size = 1) {
    StrokeStyle s;
    s.color = color;
    s.brush.size = size;
    return s;
}
int countColor(const QImage &im, QRgb c) {
    int n = 0;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
            n += im.pixel(x, y) == c;
    return n;
}
} // namespace

class EditingTests : public QObject {
    Q_OBJECT
  private slots:
    void selectionModesAreUndoableAndNotDirty() {
        auto d = doc();
        QVERIFY(!d->modified());
        QVERIFY(d->select(ops::rectMask({16, 16}, {0, 0, 8, 8}), ops::Combine::Replace));
        QVERIFY(d->hasSelection());
        QVERIFY(!d->modified()); // selection is view state, not content
        QVERIFY(d->select(ops::rectMask({16, 16}, {4, 4, 8, 8}), ops::Combine::Add));
        QCOMPARE(ops::maskBounds(d->selection()), QRect(0, 0, 12, 12));
        QVERIFY(d->select(ops::rectMask({16, 16}, {0, 0, 4, 4}), ops::Combine::Subtract));
        QCOMPARE(d->selection().constScanLine(1)[1], uchar(0));
        QVERIFY(d->select(ops::rectMask({16, 16}, {6, 6, 10, 10}), ops::Combine::Intersect));
        QCOMPARE(ops::maskBounds(d->selection()), QRect(6, 6, 6, 6));
        d->undo();
        QCOMPARE(ops::maskBounds(d->selection()), QRect(0, 0, 12, 12));
        d->redo();
        QCOMPARE(ops::maskBounds(d->selection()), QRect(6, 6, 6, 6));
        QVERIFY(d->invertSelection());
        QCOMPARE(d->selection().constScanLine(0)[0], uchar(255));
        QVERIFY(d->deselect());
        QVERIFY(!d->hasSelection());
        QVERIFY(d->selectAll());
        QCOMPARE(ops::maskBounds(d->selection()), QRect(0, 0, 16, 16));
        QVERIFY(!d->modified());
        // Zero-pixel selections retain their editing restriction until Deselect.
        QVERIFY(d->select(ops::emptyMask({16, 16}), ops::Combine::Replace));
        QVERIFY(d->hasSelection());
        QVERIFY(ops::maskEmpty(d->selection()));
        QString error;
        QVERIFY(!d->select(ops::emptyMask({8, 8}), ops::Combine::Replace, "x", &error));
    }
    void pencilStrokeMatchesClassicPaint() {
        auto a = doc(), b = doc();
        QVERIFY(a->paint(a->activeLayer(), {1, 1}, {12, 7}, qRgb(9, 8, 7)));
        a->finish();
        QVERIFY(b->stroke(b->activeLayer(), {1, 1}, {12, 7}, style(qRgb(9, 8, 7))));
        b->finish();
        QCOMPARE(a->image, b->image);
        QCOMPARE(b->historyOperations(), 1);
        b->undo();
        QCOMPARE(countColor(b->image, qRgb(9, 8, 7)), 0);
    }
    void emptySelectionsNeverExpandEditingScope() {
        auto d = doc();
        QVERIFY(d->stroke(d->activeLayer(), {0, 0}, {15, 15}, style(qRgb(90, 70, 40))));
        d->finish();
        const auto original = d->image;
        const auto square = ops::rectMask(d->image.size(), {2, 2, 6, 6});
        QVERIFY(d->select(square, ops::Combine::Replace));
        QVERIFY(d->select(square, ops::Combine::Subtract));
        QVERIFY(d->hasSelection());
        QVERIFY(ops::maskEmpty(d->selection()));
        const int history = d->historyOperations();
        QVERIFY(d->stroke(d->activeLayer(), {0, 0}, {15, 15}, style()));
        d->finish();
        QVERIFY(d->clearSelected(d->activeLayer()));
        QVERIFY(d->fill(d->activeLayer(), {4, 4}, qRgb(10, 20, 30), 255, ops::Distance::Rgb, false, false));
        ops::ShapeSpec shape;
        shape.bounds = {0, 0, 16, 16};
        shape.filled = shape.smooth = true;
        QVERIFY(d->drawShape(d->activeLayer(), shape, qRgb(1, 2, 3)));
        for (const auto &spec : std::vector<ops::Spec>{{"invert", {}}, {"flip", {}},
                                  {"gen_bricks", {{"width", 16}, {"height", 16}}}, {"directional_shading", {}}})
            QVERIFY(d->applyOperation(d->activeLayer(), spec));
        QCOMPARE(d->image, original);
        QCOMPARE(d->layers().size(), size_t(1));
        QCOMPARE(d->historyOperations(), history);
        d->undo();
        QCOMPARE(d->selection(), square);
        d->redo();
        QVERIFY(d->hasSelection());
        QVERIFY(ops::maskEmpty(d->selection()));
        QVERIFY(d->invertSelection());
        QCOMPARE(d->selection(), ops::fullMask(d->image.size()));
        QVERIFY(d->invertSelection());
        QVERIFY(ops::maskEmpty(d->selection()));
        QVERIFY(d->deselect());
        QVERIFY(!d->hasSelection());
        QVERIFY(d->applyOperation(d->activeLayer(), {"invert", {}}));
        QVERIFY(d->image != original);
    }
    void brushSizesAndFastStrokesLeaveNoGaps() {
        auto d = doc(32, 32);
        auto s = style(qRgb(0, 0, 255), 3);
        // A single jump across the canvas (fast pointer) still paints a continuous band.
        QVERIFY(d->stroke(d->activeLayer(), {2, 16}, {29, 16}, s));
        d->finish();
        for (int x = 1; x <= 30; ++x)
            for (int y = 15; y <= 17; ++y)
                QCOMPARE(at(d, x, y), qRgb(0, 0, 255));
        QCOMPARE(at(d, 2, 14), QRgb(0));
        // Many tiny segments give the same result as one long one.
        auto e = doc(32, 32);
        for (int x = 2; x < 29; ++x)
            QVERIFY(e->stroke(e->activeLayer(), {x, 16}, {x + 1, 16}, s));
        e->finish();
        QCOMPARE(d->image, e->image);
        QCOMPARE(e->historyOperations(), 1); // one continuous stroke = one undo step
    }
    void spacingIsStableAcrossPollingRates() {
        auto s = style(qRgb(255, 255, 255), 1);
        s.brush.spacing = 400; // dab every 4 pixels
        auto one = doc(32, 4), many = doc(32, 4);
        QVERIFY(one->stroke(one->activeLayer(), {0, 1}, {30, 1}, s));
        one->finish();
        for (int x = 0; x < 30; x += 3)
            QVERIFY(many->stroke(many->activeLayer(), {x, 1}, {std::min(30, x + 3), 1}, s));
        many->finish();
        QCOMPARE(one->image, many->image);
        QCOMPARE(countColor(one->image, qRgb(255, 255, 255)), 8);
    }
    void opacityDoesNotCompoundWithinAStroke() {
        auto d = doc(8, 8);
        auto s = style(qRgba(0, 0, 0, 255), 3);
        s.brush.opacity = 128;
        QVERIFY(d->stroke(d->activeLayer(), {2, 2}, {5, 2}, s));
        QVERIFY(d->stroke(d->activeLayer(), {5, 2}, {2, 2}, s)); // back over the same pixels
        d->finish();
        QCOMPARE(qAlpha(at(d, 3, 2)), 128);
    }
    void cancelRestoresPreStrokePixels() {
        auto d = doc();
        QVERIFY(d->stroke(d->activeLayer(), {0, 0}, {15, 15}, style()));
        d->finish();
        const auto before = d->image;
        QVERIFY(d->stroke(d->activeLayer(), {0, 15}, {15, 0}, style(qRgb(0, 255, 0), 2)));
        QVERIFY(d->image != before);
        d->cancelStroke();
        QCOMPARE(d->image, before);
        QCOMPARE(d->historyOperations(), 1);
    }
    void symmetryAndSelectionClipping() {
        auto d = doc();
        auto s = style();
        s.symmetry = ops::Symmetry::Both;
        QVERIFY(d->stroke(d->activeLayer(), {2, 3}, {2, 3}, s));
        d->finish();
        for (auto p : {QPoint(2, 3), QPoint(13, 3), QPoint(2, 12), QPoint(13, 12)})
            QCOMPARE(at(d, p.x(), p.y()), qRgb(255, 0, 0));
        auto c = doc();
        QVERIFY(c->select(ops::rectMask({16, 16}, {0, 0, 8, 16}), ops::Combine::Replace));
        QVERIFY(c->stroke(c->activeLayer(), {0, 5}, {15, 5}, style(qRgb(1, 1, 1), 3)));
        c->finish();
        QCOMPARE(at(c, 7, 5), qRgb(1, 1, 1));
        QCOMPARE(at(c, 8, 5), QRgb(0)); // clipped at the selection edge
    }
    void eraserColorReplaceAndPaletteLock() {
        auto d = doc();
        QVERIFY(d->stroke(d->activeLayer(), {0, 0}, {15, 0}, style(qRgb(10, 20, 30), 1)));
        auto e = style();
        e.erase = true;
        QVERIFY(d->stroke(d->activeLayer(), {0, 0}, {3, 0}, e));
        d->finish();
        QCOMPARE(at(d, 2, 0), qRgba(0, 0, 0, 0));
        QCOMPARE(at(d, 5, 0), qRgb(10, 20, 30));
        auto r = style(qRgb(200, 0, 200), 3);
        r.replaceOnly = true;
        r.replaceColor = qRgb(10, 20, 30);
        r.replaceTolerance = 0;
        QVERIFY(d->stroke(d->activeLayer(), {8, 1}, {8, 1}, r)); // covers rows 0..2
        d->finish();
        QCOMPARE(at(d, 8, 0), qRgb(200, 0, 200));
        QCOMPARE(at(d, 8, 1), QRgb(0)); // transparent pixels were not matches
        auto p = style(qRgb(250, 10, 10));
        p.palette = {qRgb(255, 0, 0), qRgb(0, 0, 0)};
        QVERIFY(d->stroke(d->activeLayer(), {0, 5}, {0, 5}, p));
        d->finish();
        QCOMPARE(at(d, 0, 5), qRgb(255, 0, 0));
    }
    void fillToleranceGlobalAndSelection() {
        auto d = doc(8, 8);
        QVERIFY(d->stroke(d->activeLayer(), {4, 0}, {4, 7}, style(qRgb(0, 0, 0))));
        d->finish();
        QVERIFY(d->fill(d->activeLayer(), {0, 0}, qRgb(0, 255, 0), 0, ops::Distance::Rgb, true, false));
        QCOMPARE(at(d, 3, 3), qRgb(0, 255, 0));
        QCOMPARE(at(d, 5, 3), QRgb(0));
        QCOMPARE(d->historyOperations(), 2);
        d->undo();
        QCOMPARE(at(d, 3, 3), QRgb(0));
        QVERIFY(d->fill(d->activeLayer(), {0, 0}, qRgb(0, 255, 0), 0, ops::Distance::Rgb, false, false));
        QCOMPARE(at(d, 5, 3), qRgb(0, 255, 0)); // global replacement crosses the wall
        auto s = doc(8, 8);
        QVERIFY(s->select(ops::rectMask({8, 8}, {0, 0, 2, 2}), ops::Combine::Replace));
        QVERIFY(s->fill(s->activeLayer(), {5, 5}, qRgb(1, 2, 3), 0, ops::Distance::Rgb, true, false));
        QCOMPARE(at(s, 1, 1), qRgb(1, 2, 3));
        QCOMPARE(at(s, 5, 5), QRgb(0));
    }
    void shapesAreOneUndoStep() {
        auto d = doc();
        ops::ShapeSpec shape;
        shape.kind = ops::ShapeKind::Ellipse;
        shape.bounds = QRectF(2, 2, 10, 8);
        QVERIFY(d->drawShape(d->activeLayer(), shape, qRgb(5, 5, 5)));
        QCOMPARE(d->historyOperations(), 1);
        QCOMPARE(d->historyLabels().last(), QString("Ellipse"));
        QVERIFY(countColor(d->image, qRgb(5, 5, 5)) > 10);
        QCOMPARE(at(d, 7, 6), QRgb(0)); // outline only
        d->undo();
        QCOMPARE(countColor(d->image, qRgb(5, 5, 5)), 0);
    }
    void layerOperationsPreserveUnrelatedLayers() {
        auto d = doc(8, 8);
        const auto base = d->activeLayer();
        QVERIFY(d->stroke(base, {0, 0}, {7, 7}, style(qRgb(9, 9, 9))));
        d->finish();
        QVERIFY(d->editLayer({LayerCommand::Add, base, "Top"}));
        const auto top = d->activeLayer();
        QVERIFY(d->stroke(top, {0, 7}, {7, 0}, style(qRgb(200, 100, 50))));
        d->finish();
        const auto bottomPixels = d->layers()[0].pixels;
        QVERIFY(d->applyOperation(top, {"invert", {}}));
        QCOMPARE(d->layers()[0].pixels, bottomPixels);
        QCOMPARE(d->layers()[1].pixels.pixel(0, 7), qRgb(55, 155, 205));
        d->undo();
        QCOMPARE(d->layers()[1].pixels.pixel(0, 7), qRgb(200, 100, 50));
        d->redo();
        QCOMPARE(d->historyLabels().last(), QString("Invert"));
        QString error;
        QVERIFY(d->editLayer({LayerCommand::Visibility, top, {}, 0}));
        QVERIFY(!d->applyOperation(top, {"invert", {}}, &error));
        QVERIFY(error.contains("Show"));
    }
    void smoothShapesRespectSelectionHistoryAndNativePixels() {
        auto d = doc();
        const auto original = d->image;
        QVERIFY(d->select(ops::rectMask({16, 16}, {0, 0, 8, 16}), ops::Combine::Replace));
        ops::ShapeSpec shape;
        shape.kind = ops::ShapeKind::Ellipse;
        shape.bounds = {1, 1, 13, 13};
        shape.filled = true;
        shape.smooth = true;
        const int history = d->historyOperations();
        QVERIFY(d->drawShape(d->activeLayer(), shape, qRgba(200, 100, 50, 192)));
        QCOMPARE(d->historyOperations(), history + 1);
        int partial = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                if (x >= 8)
                    QCOMPARE(d->image.pixel(x, y), QRgb(0));
                else
                    partial += qAlpha(d->image.pixel(x, y)) > 0 && qAlpha(d->image.pixel(x, y)) < 192;
            }
        QVERIFY(partial > 0);
        const auto result = d->image;
        d->undo();
        QCOMPARE(d->image, original);
        d->redo();
        QCOMPARE(d->image, result);
        Document reopened(QString(), QImage{});
        QString error;
        QVERIFY2(reopened.decodeNative(d->encodeNative("smooth.png", &error), "smooth.png", &error), qPrintable(error));
        QCOMPARE(reopened.image, result);
        QVERIFY(!reopened.hasSelection());
    }
    void rectangularStrokesCancelAndRejectInvalidHeight() {
        auto d = doc(32, 32);
        auto s = style(qRgb(70, 80, 90), 3);
        s.brush.height = 9;
        QVERIFY(d->stroke(d->activeLayer(), {8, 8}, {20, 8}, s));
        QCOMPARE(countColor(d->image, s.color), 15 * 9);
        d->cancelStroke();
        QCOMPARE(countColor(d->image, s.color), 0);
        QVERIFY(d->stroke(d->activeLayer(), {8, 8}, {20, 8}, s));
        d->finish();
        const auto result = d->image;
        QCOMPARE(d->historyOperations(), 1);
        d->undo();
        QCOMPARE(countColor(d->image, s.color), 0);
        d->redo();
        QCOMPARE(d->image, result);
        QString error;
        s.brush.height = 257;
        QVERIFY(!d->stroke(d->activeLayer(), {8, 8}, {8, 8}, s, &error));
        QCOMPARE(d->image, result);
    }
    void documentResizeIsUndoableAcrossLayers() {
        auto d = doc(4, 2);
        QVERIFY(d->stroke(d->activeLayer(), {0, 0}, {0, 0}, style(qRgb(1, 0, 0))));
        d->finish();
        QVERIFY(d->editLayer({LayerCommand::Add, d->activeLayer(), "B"}));
        QVERIFY(d->stroke(d->activeLayer(), {3, 1}, {3, 1}, style(qRgb(0, 0, 1))));
        d->finish();
        QVERIFY(d->select(ops::rectMask({4, 2}, {0, 0, 1, 1}), ops::Combine::Replace));
        QVERIFY(d->applyDocumentOperation({"image_rotate", {}}));
        QCOMPARE(d->image.size(), QSize(2, 4));
        for (const auto &l : d->layers())
            QCOMPARE(l.pixels.size(), QSize(2, 4));
        QCOMPARE(d->layers()[0].pixels.pixel(1, 0), qRgb(1, 0, 0));
        QCOMPARE(d->layers()[1].pixels.pixel(0, 3), qRgb(0, 0, 1));
        QVERIFY(!d->hasSelection()); // document transforms clear the selection
        d->undo();
        QCOMPARE(d->image.size(), QSize(4, 2));
        QCOMPARE(d->layers()[1].pixels.pixel(3, 1), qRgb(0, 0, 1));
        QVERIFY(d->hasSelection());
        d->redo();
        QVERIFY(d->applyDocumentOperation({"resize_canvas", {{"width", 6}, {"height", 6}}}));
        QCOMPARE(d->image.size(), QSize(6, 6));
        QVERIFY(d->applyDocumentOperation({"trim", {}}));
        QCOMPARE(d->image.size(), QSize(2, 4));
        QString error;
        QVERIFY(!d->applyDocumentOperation({"invert", {}}, &error));
        QVERIFY(!d->applyDocumentOperation({"resize_image", {{"width", 5000}}}, &error));
    }
    void floatingCommitsAndClipboardFlows() {
        auto d = doc(8, 8);
        const auto base = d->activeLayer();
        QVERIFY(d->stroke(base, {1, 1}, {2, 1}, style(qRgb(7, 7, 7))));
        d->finish();
        QVERIFY(d->select(ops::rectMask({8, 8}, {1, 1, 2, 1}), ops::Combine::Replace));
        // Copy keeps transparency outside the selection.
        auto copied = d->selectedPixels(base);
        QCOMPARE(copied.pixel(1, 1), qRgb(7, 7, 7));
        QCOMPARE(qAlpha(copied.pixel(5, 5)), 0);
        // Move: lift from (1,1) and drop at (4,5), moving the selection too.
        const auto lift = d->selection();
        const auto moved = ops::wrapOffset(copied, 3, 4);
        const auto movedSelection = ops::wrapOffset(lift.convertToFormat(QImage::Format_RGBA8888), 3, 4)
                                        .convertToFormat(QImage::Format_Grayscale8);
        QVERIFY(d->commitFloating(base, moved, &lift, "Move", false, nullptr, &movedSelection));
        QCOMPARE(at(d, 4, 5), qRgb(7, 7, 7));
        QCOMPARE(at(d, 1, 1), qRgba(0, 0, 0, 0));
        QCOMPARE(d->historyOperations(), 3); // stroke, select, one compound move
        d->undo();
        QCOMPARE(at(d, 1, 1), qRgb(7, 7, 7));
        QCOMPARE(at(d, 4, 5), QRgb(0));
        QCOMPARE(ops::maskBounds(d->selection()), QRect(1, 1, 2, 1));
        // Paste as new layer.
        QVERIFY(d->commitFloating(base, moved, nullptr, "Paste", true));
        QCOMPARE(int(d->layers().size()), 2);
        QCOMPARE(d->layers()[1].pixels.pixel(4, 5), qRgb(7, 7, 7));
        QCOMPARE(d->layers()[0].pixels.pixel(1, 1), qRgb(7, 7, 7));
        // Cut = clear selected.
        QVERIFY(d->clearSelected(base));
        QCOMPARE(d->layers()[0].pixels.pixel(1, 1), qRgba(0, 0, 0, 0));
    }
    void mergeDownAndFlatten() {
        auto d = doc(4, 4);
        const auto base = d->activeLayer();
        QVERIFY(d->stroke(base, {0, 0}, {3, 0}, style(qRgb(255, 0, 0))));
        d->finish();
        QVERIFY(d->editLayer({LayerCommand::Add, base, "Top"}));
        const auto top = d->activeLayer();
        QVERIFY(d->stroke(top, {0, 0}, {1, 0}, style(qRgb(0, 0, 255))));
        d->finish();
        QVERIFY(d->editLayer({LayerCommand::Opacity, top, {}, 128}));
        const auto composite = d->image;
        QVERIFY(d->mergeDown(top));
        QCOMPARE(int(d->layers().size()), 1);
        QCOMPARE(d->image, composite);
        d->undo();
        QCOMPARE(int(d->layers().size()), 2);
        QCOMPARE(d->layers()[1].opacity, 128);
        QVERIFY(d->editLayer({LayerCommand::Add, top, "Third"}));
        QVERIFY(d->flatten());
        QCOMPARE(int(d->layers().size()), 1);
        QCOMPARE(d->image.pixel(0, 0), composite.pixel(0, 0));
        d->undo();
        QCOMPARE(int(d->layers().size()), 3);
    }
    void shadingLayerIsNonDestructive() {
        auto d = doc(8, 8);
        const auto base = d->activeLayer();
        QVERIFY(d->applyOperation(base, {"fill", {{"color", "#FF808080"}}}));
        const auto original = d->layers()[0].pixels;
        QVERIFY(d->applyOperation(base, {"directional_shading", {{"height_source", "Alpha bevel"}}}));
        QCOMPARE(int(d->layers().size()), 2);
        QCOMPARE(d->layers()[0].pixels, original);
        QVERIFY(d->layers()[1].extensions.contains("nova.effect"));
        QCOMPARE(d->historyLabels().last(), QString("Directional Shading"));
        d->undo();
        QCOMPARE(int(d->layers().size()), 1);
    }
    void historyRemainsBounded() {
        auto d = doc(64, 64);
        d->setHistoryLimits(256 * 1024, 2048);
        QVERIFY(d->applyOperation(d->activeLayer(), {"fill", {{"color", "#FF808080"}}}));
        for (int i = 0; i < 40; ++i)
            QVERIFY(d->applyOperation(d->activeLayer(), {"noise", {{"seed", i}, {"monochrome", false}}}));
        QVERIFY(d->historyBytes() <= 256 * 1024);
        QVERIFY(d->historyOperations() < 40);
        QVERIFY(d->canUndo());
        d->jumpHistory(0);
        QVERIFY(d->historyCursor() == 0);
    }
    void nativeRoundTripAfterTransforms() {
        QTemporaryDir tmp;
        const auto project = tmp.path() + "/pack", png = project + "/assets/minecraft/textures/block/a.png";
        QDir().mkpath(QFileInfo(png).absolutePath());
        QVERIFY(savePng(png, blank(16, 16)));
        DocumentSession session;
        session.setProject(project);
        auto d = session.open(png);
        QVERIFY(d);
        QString error;
        QVERIFY(session.apply(d, d->activeLayer(), {"gen_bricks", {{"width", 16}, {"height", 16}}}, &error));
        QVERIFY(session.addLayer(d, "Shade", blank(16, 16), {{"nova.generator", QJsonObject{{"id", "gen_bricks"}}}}));
        QVERIFY(session.apply(d, d->activeLayer(), {"resize_image", {{"width", 32}, {"height", 32}}}, &error));
        QVERIFY(session.select(d, ops::rectMask({32, 32}, {0, 0, 4, 4}), ops::Combine::Replace));
        QVERIFY(session.dispatch(d, Command::SaveNative, {}, false, &error));
        const auto composite = d->image;
        const auto ids = d->layers()[1].id;
        session.close(d);
        auto reopened = session.open(png, &error);
        QVERIFY2(reopened, qPrintable(error));
        QCOMPARE(reopened->image.size(), QSize(32, 32));
        QCOMPARE(reopened->image, composite);
        QCOMPARE(reopened->layers()[1].id, ids);
        QVERIFY(reopened->layers()[1].extensions.contains("nova.generator"));
        QVERIFY(!reopened->hasSelection()); // transient selection policy: never persisted
        // PNG export carries the transformed composite exactly.
        QVERIFY(session.dispatch(reopened, Command::ExportPng, {}, false, &error));
        QCOMPARE(loadPng(png), composite);
        // Recovery snapshot of a resized layered document restores identical pixels.
        RecoveryStore store(tmp.path() + "/recovery");
        QVERIFY(session.apply(reopened, reopened->activeLayer(), {"invert", {}}, &error));
        QVERIFY(store.writeDocument(*reopened->recoverySnapshot(), project, &error));
        auto entry = store.load(reopened->id());
        QVERIFY(entry.error.isEmpty());
        Document restored(png, QImage{});
        QVERIFY(restored.decodeNative(entry.nativeData, "recovery.png", &error));
        QCOMPARE(restored.image, reopened->image);
    }
    void phase2c2DocumentsRemainCompatible() {
        // Fixture written by the Phase 2-C1/C2 code (commit 3316c93) with a C2-only build.
        const auto file = QFINDTESTDATA("fixtures/c2-legacy.nvdoc");
        QVERIFY2(!file.isEmpty(), "missing tests/fixtures/c2-legacy.nvdoc");
        QFile f(file);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto bytes = f.readAll();
        const QString asset = "assets/minecraft/textures/block/legacy.png";
        Document d("legacy.png", QImage{});
        QString error;
        QVERIFY2(d.decodeNative(bytes, asset, &error), qPrintable(error));
        QCOMPARE(int(d.layers().size()), 3);
        QCOMPARE(d.layers()[0].name, QString("Base"));
        QCOMPARE(d.layers()[1].opacity, 170);
        QVERIFY(!d.layers()[2].visible);
        QCOMPARE(d.activeLayer(), d.layers()[1].id);
        QCOMPARE(d.image, loadPng(QFINDTESTDATA("fixtures/c2-legacy-composite.png")));
        // Phase 2-C3 writes the same schema-1 container byte for byte.
        QCOMPARE(d.encodeNative(asset), bytes);
        // C3 edits round-trip and keep every layer identity and the hidden layer intact.
        QVERIFY(d.applyOperation(d.activeLayer(), {"invert", {}}, &error));
        QVERIFY(d.applyDocumentOperation({"resize_canvas", {{"width", 20}, {"height", 18}}}, &error));
        Document back("legacy.png", QImage{});
        QVERIFY(back.decodeNative(d.encodeNative(asset), asset, &error));
        QCOMPARE(back.image, d.image);
        for (size_t i = 0; i < 3; ++i) {
            QCOMPARE(back.layers()[i].id, d.layers()[i].id);
            QCOMPARE(back.layers()[i].pixels, d.layers()[i].pixels);
        }
    }
    void sessionExecuteJsonCommands() {
        DocumentSession session;
        auto d = doc(8, 8);
        QVERIFY(session.adopt(d));
        QString error;
        QVERIFY(session.execute(d, {{"command", "select_rect"}, {"x", 0}, {"y", 0}, {"width", 4}, {"height", 8}}, &error));
        QVERIFY(session.execute(d, {{"command", "operation"}, {"operation", "fill"}, {"params", QJsonObject{{"color", "#FF112233"}}}}, &error));
        QCOMPARE(at(d, 1, 1), qRgb(0x11, 0x22, 0x33));
        QCOMPARE(at(d, 6, 1), QRgb(0));
        QVERIFY(session.execute(d, {{"command", "deselect"}}, &error));
        QVERIFY(session.execute(d, {{"command", "stroke"}, {"color", "#FFFFFFFF"},
                                    {"points", QJsonArray{QJsonArray{0, 7}, QJsonArray{7, 7}}}}, &error));
        QCOMPARE(at(d, 5, 7), qRgb(255, 255, 255));
        QVERIFY(session.execute(d, {{"command", "undo"}}, &error));
        QCOMPARE(at(d, 5, 7), QRgb(0));
        QVERIFY(!session.execute(d, {{"command", "explode"}}, &error));
        QVERIFY(!session.execute(d, {{"command", "operation"}, {"operation", "invert"}, {"params", QJsonObject{{"x", 1}}}}, &error));
        QVERIFY(session.execute(d, {{"command", "select_color"}, {"x", 0}, {"y", 0}}, &error));
        QVERIFY(d->hasSelection());
    }
};
QTEST_GUILESS_MAIN(EditingTests)
#include "editing_test.moc"
