// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "revision.h"
#include <QStringList>
namespace nova {
enum class PairAction { Retry, Rollback };
struct PairStatus {
    QString journal, png, state, error;
    bool valid = false, complete = false;
};
using PairWriter = std::function<bool(const QString &, const QByteArray &, const Revision &, QString *)>;
// Each replacement is atomic. The pair is journaled and recoverable, NOT atomic.
QString stageAnimationPair(const QString &png, const QByteArray &pixels, const QByteArray &metadata,
                           const Revision &expectedPng, const Revision &expectedMetadata,
                           QString *error = nullptr, const PairWriter &stagingWriter = {});
PairStatus inspectAnimationPair(const QString &journal);
bool finishAnimationPair(const QString &journal, PairAction action, QString *error = nullptr,
                         const std::function<bool()> &afterFirstReplacement = {});
QStringList pendingAnimationPairs(const QString &png);
} // namespace nova
