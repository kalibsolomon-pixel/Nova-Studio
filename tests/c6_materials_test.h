// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
class MaterialsTest : public QObject {
    Q_OBJECT
  private slots:
    void independentTimingAndLifecycle();
    void gpuReuseAndCleanup();
    void projectState();
    void editableCatalogPriority();
    void liveMaterialSwitchAndClose();
    void tallAnimatedMaterialSheet();
    void shadowedResourceInspection();
};
