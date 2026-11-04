// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <optional>

// Phase 4-G: offering an authorized connection to an AI agent without editing its configuration.
//
// Nova still issues a single-use, five-minute connection code only after the user chooses
// Allow in the MCP Integration window. The code is written to a per-user file that only the
// current user (at medium integrity or above) can read, so an agent's configuration can name
// nova-mcp.exe without embedding a secret. The bridge reads the file when NOVA_MCP_CODE is not
// set. Nova withdraws the file when the code is used, expires, is revoked or Nova exits.
namespace nova::mcp::handoff {
struct Pending {
    QString code, project;
    qint64 expiresUtcMs = 0;
    quint32 process = 0;
};
// NOVA_MCP_HANDOFF_DIR, or %LOCALAPPDATA%\Nova Studio\Nova Studio\mcp.
QString directory();
QString filePath();
bool publish(const Pending &pending, QString *error = nullptr);
// Removes the file only while it still holds this code (another Nova window may have replaced it).
void withdraw(const QString &code);
std::optional<Pending> read(QString *error = nullptr);
}
