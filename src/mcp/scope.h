// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <memory>

namespace nova::mcp {
bool logicalAsset(const QString &path);

class Scope {
    struct Handles;
    std::unique_ptr<Handles> handles;
  public:
    explicit Scope(const QString &root, const QByteArray &identity = {});
    ~Scope();
    QByteArray identity() const;
    bool vacant(const QString &relative);
    std::unique_ptr<QIODevice> open(const QString &relative);
};
}
