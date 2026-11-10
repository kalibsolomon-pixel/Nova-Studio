// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "project.h"
#include <QWidget>
namespace nova {
QString newProjectDialog(QWidget *parent);
void projectPropertiesDialog(QWidget *parent, const QString &root);
// Edits the pack's style notes (.nova/project.json). Returns true when they were saved.
bool styleNotesDialog(QWidget *parent, const QString &root);
QString styleNotesSummary(const StyleNotes &notes);
} // namespace nova
