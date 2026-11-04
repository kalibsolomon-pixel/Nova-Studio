// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
class UvTest : public QObject {
    Q_OBJECT
    static QString fixture(const QString &root);
  private slots:
    void guardedHistoryAndPreservation();
    void inheritedConversion();
    void interactivePixelCoordinates();
};
