// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
class ModelTest : public QObject {
    Q_OBJECT
    static void write(const QString &, const QString &, const QByteArray &);
    static QByteArray box();
  private slots:
    void locations();
    void inheritanceAliasesGraphAndIncremental();
    void failuresAndProfiles();
    void geometryAndUvs();
    void blockstatesAndItems();
    void rescaleUvlockAndModernShading();
};
