// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>

namespace nova::diagnostics {
// Local only. At most two 1 MiB logs; no project scanning or network activity.
bool start(const QString &directory);
QString logPath();
QJsonObject report();
bool exportReport(const QString &destination, QString *error = nullptr);
}
