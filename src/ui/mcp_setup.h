// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <functional>
class QWidget;

namespace nova {
// Phase 4-G: the Agent Setup page of Resources › MCP Integration. It configures AI agents to
// start nova-mcp.exe and checks that setup. It never grants project access: that stays on the
// Access tab. `connection` reports the live connection for diagnostics ("" when none).
QWidget *createMcpSetupPage(QWidget *parent, std::function<QString()> connection);
// Summary line shared by the dialog header: bridge availability plus the access state.
QString mcpBridgeSummary();
}
