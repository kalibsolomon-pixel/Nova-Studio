// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>

class QDialog;
class QWidget;

// Nova Studio's license notices (GPL-3.0 "Appropriate Legal Notices") and the
// Licenses and Source Code dialog. The license texts are embedded as Qt resources, so
// they can be shown even when the licenses folder is missing.
namespace nova::legal {
// Copyright line, license, no-warranty statement and where to read the license.
QString notice();
// An embedded document: "LICENSE" (GPL-3.0 text), "COPYRIGHT.md" or "THIRD_PARTY_NOTICES.md".
QString document(const QString &name);
// The folder next to the executable that holds the corresponding source archive, if any.
QString sourceFolder();
// Where to find the source code of this exact build, as plain text.
QString sourceNotice();
// Tabs: Nova Studio, GNU GPL v3, Third-party notices, Source code. Deleted on close.
QDialog *createLicensesDialog(QWidget *parent);
} // namespace nova::legal
