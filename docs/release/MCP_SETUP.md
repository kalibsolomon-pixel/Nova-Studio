# Connecting an AI agent (MCP)

Nova Studio includes `nova-mcp.exe`, a local [Model Context Protocol](https://modelcontextprotocol.io)
server. With it, an AI agent on your PC can inspect the resource pack open in Nova Studio and, only
if you allow editing, change it through Nova Studio's normal undo, save and recovery rules.

## How access works

- **Setup** tells an agent where `nova-mcp.exe` is. It does not grant access to anything.
- **Access is off until you choose Allow** in Resources › MCP Integration › Access, for the
  project that is open now. **Allow read-only** lets the agent inspect; **Allow editing…** also lets
  it change open documents (you confirm this separately).
- An authorization is used by one agent session. It ends when the agent disconnects, when you open
  another project, when you choose **Revoke access**, or after five minutes if no agent connects.
- Agents never get file-system access outside the open pack through Nova Studio.

## Set up an agent

Open **Resources › MCP Integration** (or press Ctrl+K and type `mcp`), then the **Agent Setup** tab.
Choose your agent. Nova Studio shows whether it found the agent and whether Nova Studio is already
configured.

| Agent | What **Add Nova Studio…** does | Then |
| --- | --- | --- |
| Claude Desktop | Adds a `nova-studio` entry to `claude_desktop_config.json`, leaving everything else in the file unchanged, after backing it up. Quit Claude Desktop first. | Choose Allow, then start Claude Desktop. |
| Claude Code | Runs `claude mcp add --scope user nova-studio -- "<path>\nova-mcp.exe"`. | Choose Allow, then start Claude Code, or run `/mcp` and reconnect `nova-studio`. |
| Codex | Runs `codex mcp add nova-studio -- "<path>\nova-mcp.exe"`. | Choose Allow, then start a new Codex session. |
| Other MCP clients | Nothing automatic. **Copy configuration** gives the command and a JSON entry. | Add a stdio server that runs `nova-mcp.exe` with no arguments. |

Nova Studio always shows what it will change and asks first. If a configuration file is not valid,
is ambiguous (for example, two possible Claude Desktop files), or the agent's command-line tool is
not found, Nova Studio changes nothing and shows the configuration to copy instead. Backups are kept
in `%LOCALAPPDATA%\Nova Studio\Nova Studio\mcp\client-backups`.

## Each session

1. In Nova Studio, open the pack, then **Resources › MCP Integration › Access › Allow read-only**
   (or **Allow editing…**).
2. Within five minutes, start your agent or reconnect its `nova-studio` server.
3. The Access tab shows **Client connected** with the agent's name. Workflow activity and **Review
   changes** list what the agent did; every edit can be undone.

## Diagnostics

**Agent Setup › Run diagnostics** checks, without granting access:

- `nova-mcp.exe` exists next to Nova Studio and starts;
- its version matches Nova Studio;
- its protocol self-test answers and declares the 26 tools;
- the selected agent's configuration points to this `nova-mcp.exe`;
- whether an agent is connected right now.

These checks are local. Only **Client connected** on the Access tab shows a real agent session.

## Troubleshooting

- **The agent says the server failed or disconnected:** choose Allow in Nova Studio first, then
  reconnect within five minutes. The agent's log shows the reason, for example "Nova Studio has not
  offered a connection".
- **Configured for a different copy of Nova Studio:** you moved or reinstalled Nova Studio. Choose
  **Update Nova Studio…**.
- **Claude Desktop is running:** quit it from the notification area, then Check again.
- **Advanced:** instead of the automatic offer, a client can pass the code from **Copy connection
  code** in the `NOVA_MCP_CODE` environment variable.

## Uninstalling

Uninstalling Nova Studio does not change your agents. Remove their `nova-studio` entry yourself, or
they will report that it failed to start.
