#pragma once
#include <QImage>
#include <QMap>
#include <QPointF>
#include <QStringList>
#include <optional>
#include <vector>

namespace nova {
std::optional<QPoint> coordinate(QPointF point, QPointF origin, double zoom, QSize size);
double zoomBy(double current, double delta);
QVector<QPoint> line(QPoint a, QPoint b);
QImage loadPng(const QString &path, QString *error = nullptr);
bool savePng(const QString &path, const QImage &image, QString *error = nullptr);
struct Change {
    int index;
    QRgb before, after;
};
class Document {
    std::vector<std::vector<Change>> history;
    QMap<int, Change> stroke;
    int cursor = 0;
    std::optional<int> saved = 0;
    void apply(bool forward);

  public:
    QString path;
    QImage image;
    QRect dirty;
    Document(QString path, QImage image);
    bool modified() const;
    void draw(QPoint a, QPoint b, QRgb color);
    void finish();
    void undo();
    void redo();
    bool save(QString *error = nullptr);
};
struct Index {
    QStringList files;
    QMap<QString, QStringList> directories;
    QString error;
    static Index scan(const QString &root);
};
struct Navigation {
    QString directory, selected, query;
    bool grid = false, reverse = false;
    QStringList entries(const Index &index) const;
};
} // namespace nova
