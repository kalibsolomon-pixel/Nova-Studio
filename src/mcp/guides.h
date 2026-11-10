// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// Read-only, versioned resource-pack guidance served as MCP resources (nova://rules/<version>/<topic>)
// and through nova.guide.get. Both read the same text from guide(), so they cannot diverge.
// Facts quoted for a version are checked against tests/fixtures/guides/minecraft-<version>-facts.json,
// which scripts/verify-guide-facts.py derives from the official client resources.
namespace nova::mcp::guides {
struct Guide {
    QString uri, topic, version, title, text;
};
struct Lookup {
    Guide guide;
    QString code, message; // set when no guide matches; never a guide for another version
    QJsonObject details;
};
QStringList guideVersions();          // versions with verified guides
QStringList novaVersions();           // versions Nova can target (src/app/project.cpp gameVersions())
QStringList topics();
QString title(const QString &topic);
QString uri(const QString &version, const QString &topic);
Lookup guide(const QString &version, const QString &topic);
// nova://rules/<version>/<topic> → version and topic; false for any other form.
bool parseUri(const QString &uri, QString *version, QString *topic);
QString indexText();
QJsonObject catalog();                // nova.guide.list result and nova://rules/index metadata
QJsonArray resources();               // resources/list entries (guides, index, style notes)
QJsonArray resourceTemplates();
QString instructions();               // initialize instructions
constexpr auto IndexUri = "nova://rules/index";
constexpr auto StyleNotesUri = "nova://project/style-notes";
}
