// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
class GroupTransformTest : public QObject {
    Q_OBJECT
  private slots:
    void previewCommitHistoryAndRecovery();
    void sharedPivotMaskAndHidden();
    void budgetsAndStalePreview();
};
