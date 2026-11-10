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

## Saving, guides and style notes

- **Edits are drafts until saved.** An agent's edits change the open documents in Nova Studio. Use
  **Save** (Ctrl+S) for the active document or **File › Save All** (Ctrl+Alt+Shift+S) to write every
  unsaved document into the pack; the status bar shows how many are unsaved. An agent with editing
  access can also save with `nova.drafts.save` and `nova.drafts.save_all`; it can only save a document
  to its own resource path, and the result lists what was saved and what was not.
- **Save writes the pack.** Saving a texture writes its PNG into the pack. Layered textures also save
  their editable layers, and animations write the PNG and its `.png.mcmeta` (one column) plus their
  editable source. If any part fails, the document stays unsaved and keeps its recovery copy.
  **Save All is not atomic:** each document is saved separately and the result shows which failed.
- **Compact texture editing.** With editing access, an agent can paint a whole region from a palette
  (`nova.texture.put_pixels`), replace colours (`nova.texture.palette_remap`), recolour by brightness
  (`nova.texture.gradient_map`) or erase (`nova.texture.erase`) in one call each. Each call is one
  undoable step on the open document and follows its selection and active layer; like every agent
  edit, it is not saved until you or the agent save. Exact rules and schemas:
  [MCP_TEXTURE_TOOLS.md](MCP_TEXTURE_TOOLS.md).
- **Vanilla textures, relationships and validation (read-only).** Any connected agent can list and
  read the vanilla textures in Nova's verified local cache (`nova.vanilla.list`,
  `nova.vanilla.texture`), see how an asset is selected and used (`nova.asset.relationships`), and
  validate saved files together with open unsaved drafts (`nova.pack.validate`). The cache holds
  textures only; vanilla models are available only from a verified vanilla source you add under
  **Models › Resource stack**. Agents cannot download a version: add it in Nova (Resources ›
  Vanilla Baseline). Details: [MCP_ASSET_TOOLS.md](MCP_ASSET_TOOLS.md).
- **Guides.** Agents can read Nova's resource-pack guides (`nova.guide.get`, or the MCP resources
  `nova://rules/<version>/<topic>`). Guides exist for Minecraft Java 1.21.11 only, checked against
  the official 1.21.11 client; other versions return "no verified guide".
- **Style notes.** **File › Project Properties › Edit style notes…** stores an optional style brief
  (theme, audience, palette, things to preserve) in `.nova/project.json`. Agents can read it but not
  change it, and it is not exported with the pack.

## Agent Session (ACP, experimental)

Agent Session is experimental and off by default. Turn it on in **Edit › Preferences ›
Experimental**; see KNOWN_LIMITATIONS.md for what has and has not been tested.

**Resources › Agent Session (ACP, Experimental)…** starts an agent that speaks the Agent Client Protocol, such as
OpenCode's `opencode acp`, and attaches this `nova-mcp.exe` to that one session. Choose the agent's
`.exe` (not a `.cmd` shim), then **Start read-only session…**; the question names the program, the
project and the access, and nothing starts until you agree.

- Only a bridge started by that agent can use the access; Nova refuses any other program.
- **Allow editing…** becomes available once the read-only session has connected. It asks again and
  restarts the agent with editing access, which includes saving into the pack.
- **Revoke Nova access** ends Nova access but leaves the agent running; **Stop session**, closing the
  window, switching projects or the agent exiting ends the agent, everything it started, and the access.
- The agent works in an empty temporary folder, not in the project. Its own tools (files, shell, web)
  follow its own settings; Nova shows its permission requests and never answers them for you.
- The window shows the conversation: your messages, the agent's replies and, in lighter text, its
  reasoning. Tool activity is not listed there; edits made through Nova appear in **MCP Integration ›
  Review changes**, and the session and access lines show what the agent may do.
- Nova keeps the conversation in the window only and passes no connection code to the agent.

## Diagnostics

**Agent Setup › Run diagnostics** checks, without granting access:

- `nova-mcp.exe` exists next to Nova Studio and starts with only the Windows folders on `PATH`, as an
  agent with an unrelated `PATH` would start it;
- its version matches Nova Studio;
- its protocol self-test answers and declares its tools;
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
- **An agent cannot create an asset:** the error names the cause and the pack-relative path at fault.
  `missing_directory` — the folder does not exist; Nova does not create folders, so create it in the
  pack first. `asset_conflict` — a file already uses the path; Nova never overwrites it.
  `unsafe_asset` — a folder on the path is a link, a junction or not an ordinary folder.
  `access_denied` — Windows refused access to that folder or file. `stale_index` with
  `details.reason` `stale_index_entry` — Nova's index listed a file that is no longer on disk; Nova
  re-indexes the project, and the agent retries after `nova.project.info` reports a new
  `indexRevision`.
- **Advanced:** instead of the automatic offer, a client can pass the code from **Copy connection
  code** in the `NOVA_MCP_CODE` environment variable.

## Uninstalling

Uninstalling Nova Studio does not change your agents. Remove their `nova-studio` entry yourself, or
they will report that it failed to start.
