#pragma once
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
namespace nova {
struct Metrics {
    QMap<QString, QJsonArray> samples;
    QJsonObject counters;
    QJsonObject environment;
    void record(const QString &name, const QElapsedTimer &timer);
    void count(const QString &name, qint64 value);
    bool write(const QString &path) const;
};
bool benchmark(const QString &root, const QString &output);
} // namespace nova
