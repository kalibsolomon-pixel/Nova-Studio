// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
class StreamingTest : public QObject {
    Q_OBJECT
  private slots:
    void uniqueSpoolRoundtripAndRecovery();
    void guardedStreamFailureAndConflict();
};
