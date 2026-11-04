// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "project.h"
#include <QWidget>
namespace nova {
QString newProjectDialog(QWidget *parent);
void projectPropertiesDialog(QWidget *parent, const QString &root);
} // namespace nova
