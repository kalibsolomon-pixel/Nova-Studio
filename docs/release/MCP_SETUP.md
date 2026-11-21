# Connecting an AI agent (MCP)

Nova Studio includes `nova-mcp.exe`, a local [Model Context Protocol](https://modelcontextprotocol.io)
server. With it, an AI agent on your PC can inspect the resource pack open in Nova Studio and, only
if you allow editing, change it through Nova Studio's normal undo, save and recovery rules.

## How access works

- **Setup** tells an agent where `nova-mcp.exe` is. It does not grant access to anything.
- **Access is off until you choose Allow** in Resources › MCP Integration › Access, for the
  project that is open now. **Allow read-only** lets the agent inspect; **Allow editing…** also lets
  it open textures, change documents and initialize an authorized pack folder (you confirm this
  separately).
- An authorization stays on until you choose **Revoke access**, open another project, or close Nova
  Studio. One agent is connected at a time; when it disconnects, Nova Studio offers a fresh
  connection code so the agent's next session reconnects on its own.
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
2. Start your agent or reconnect its `nova-studio` server. This works at any time while the access
   is on; after a disconnect the agent reconnects with the freshly offered code.
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
- **Texture editing.** With editing access, an agent can paint a whole region from a palette
  (`nova.texture.put_pixels`), replace colours (`nova.texture.palette_remap`), recolour by brightness
  (`nova.texture.gradient_map`) or erase (`nova.texture.erase`) in one call each. Combine these tools
  with native adjustments, generators and resize commands in a batch or workflow stage. Use
  `nova.texture.adjust_bulk` for up to 64 related textures. Each texture has its own result, revision
  and undo step. Exact rules and schemas are in
  [MCP_TEXTURE_TOOLS.md](MCP_TEXTURE_TOOLS.md).
- **Texture workflow.** Read pack or cached vanilla pixels with `nova.texture.read` and plan a palette.
  Edit using the current revision, opening the texture with `nova.asset.open` if needed. Use
  `nova.asset.preview_image` with `compare` to review the edit and `nova.texture.analyze` to check
  contrast, seams and layout. Request `returnPreview` during an edit to get an enlarged image immediately.
  An agent sees and sets the selection that limits its edits (`select_rect`, `select_color`, `deselect`
  and others), chooses the layer it paints with `layer_select`, and has Nova's image operations
  (blurs, curves, flips, rotation, crop, trim, canvas resize, alpha cleanup, normal maps and more).
  `nova.edit.undo` and `nova.edit.redo` move up to 32 of its own steps in one call.
- **Documents.** `nova.asset.open` opens pack textures and pack models; `nova.asset.close` closes a
  document that has no unsaved changes and refuses one that has (Nova never discards edits for an agent).
- **Agent efficiency.** An agent may send several calls at once: Nova runs them one at a time, in order,
  and refuses only a sixth outstanding call (`busy`). Long bulk and workflow edits report progress when
  the client asks for it. Every tool carries MCP annotations (a title, read-only, destructive — only the
  two saves — and idempotent hints). Nova also offers workflow-starter prompts (`retexture`, `audit-pack`,
  `recolor-set`) and completes guide versions and topics for `nova://rules/{version}/{topic}`.
- **Response size.** Edits accept `include: "summary"` to omit editor state. Clients that read only
  text can pass `verbosity: "full"` for the complete JSON result. The default text summary keeps
  essential handles and revisions.
- **Starting a pack or copying a texture.** With editing permission, an agent can initialize an
  authorized folder with Java 1.21.11 pack metadata using `nova.pack.bootstrap`. Use
  `nova.asset.create` with `source` to copy a pack or cached vanilla texture and its animation into
  an unsaved draft.
- **Vanilla textures, relationships and validation (read-only).** Any connected agent can list and
  read the vanilla textures in Nova's verified local cache (`nova.vanilla.list`,
  `nova.vanilla.texture`), see how an asset is selected and used (`nova.asset.relationships`), and
  validate saved files together with open unsaved drafts (`nova.pack.validate`). A baseline
  downloaded or repaired with this version also holds vanilla models, blockstates and item
  definitions, so vanilla parents resolve without setup; a verified vanilla source you add under
  **Models › Resource stack** takes precedence. Agents cannot download a version: add it in Nova (Resources ›
  Vanilla Baseline). Details: [MCP_ASSET_TOOLS.md](MCP_ASSET_TOOLS.md).
- **Guides.** Agents can read Nova's resource-pack guides, including `pixel-art` (`nova.guide.get`, or the MCP resources
  `nova://rules/<version>/<topic>`). Guides exist for Minecraft Java 1.21.11 only, checked against
  the official 1.21.11 client; other versions return "no verified guide".
- **Style notes.** **File › Project Properties › Edit style notes…** stores an optional style brief
  (theme, audience, palette, things to preserve) in `.nova/project.json`. Agents can read it but not
  change it, and it is not exported with the pack.

## Agent Session (ACP)

Open **Resources › Agent Session…**, search for **Agent Session**, or choose
**Open Agent Session…** from **MCP Integration**. Open a resource pack before starting a session.

The agent picker detects these agents on your PATH:

| Agent | Install (requires Node.js and npm) | Nova runs |
| --- | --- | --- |
| OpenCode | `npm install -g opencode-ai` | `opencode acp` |
| Claude Code | `npm install -g @agentclientprotocol/claude-agent-acp` | `claude-agent-acp` |
| Codex | `npm install -g @agentclientprotocol/codex-acp` | `codex-acp` |
| Gemini CLI | `npm install -g @google/gemini-cli` | `gemini --acp` |

If an agent is missing, choose **Copy install command**, run it in a terminal, then choose
**Check again**. Restart Nova Studio instead if you just installed Node.js, because the install
changed your PATH. Sign in with the agent’s advertised method or follow the login command shown
by Nova.

The Claude Code and Codex adapters moved from `@zed-industries` to `@agentclientprotocol` on npm;
the old packages no longer receive updates. If Nova finds an old adapter, the picker shows
**Update needed** and **Copy update commands** copies an `npm uninstall` for the old package
followed by the `npm install` for its replacement. The old adapter still starts if you need it.
Gemini CLI releases before 0.33 do not accept `--acp`; update Gemini CLI or use **Custom** with
`--experimental-acp`.

**Custom** accepts an absolute path to an `.exe` or a standard npm `.cmd` launcher. Put arguments
containing spaces in double quotes. Nova resolves npm launchers to `node.exe` and their package
script. Nova saves the agent choice, custom path, and arguments in its settings.

- **Start read-only…** asks you to approve the program, project, and access. Only a bridge
  from that agent’s process tree can use the grant. **Allow editing…** asks separately and
  restarts the agent with access to edit and save in the pack. If the agent can load sessions,
  it continues the same conversation in the same folder; otherwise the new session does not
  remember earlier messages, and the confirmation says so.
- **Revoke access** leaves the agent running. **Stop session**, closing the window, switching
  projects, or the agent exiting ends the agent session and its Nova access.
- The agent starts in an empty temporary folder. Its file, shell, and web tools follow its
  settings. When the agent requests permission, Nova presents its choices in order, including
  any always-allow or always-reject options the agent supplies. A request the agent withdraws
  disappears.
- When the agent offers modes or settings (such as its model), pickers appear beside the
  session status. Choosing a mode that lets the agent act without asking (for example,
  bypass permissions, full access, or auto-approve) asks you first. Modes change only the
  agent’s own tools; Nova’s read-only or editing access stays as you granted it.
- The session status also shows the conversation title and, when the agent reports them,
  context use and cost.
- The conversation shows plans, collapsible reasoning and tool cards, and formatted replies.
  Tool cards name the kind of action and the files involved. Links the agent sends are shown
  but not opened or fetched; images and audio appear as a short note.
  Enter sends, Shift+Enter inserts a line break, and Up recalls previous prompts. Type `/` to
  list the agent’s commands. **Mention the open asset** adds the pack path of the document
  open in Nova to your next message, so the agent can inspect it with Nova’s tools.
  **Cancel turn** asks the agent to stop its current response. Edits through Nova appear in
  **MCP Integration › Review changes**.
- **Show agent output** displays the last 16 KB of the agent’s standard error. Nova keeps that
  output and the conversation in memory and passes no connection code to the agent.

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
   start or reconnect the server; access stays on until you revoke it, so an agent that reconnected
   on its own needs no new Allow. The agent's log shows the reason, for example "Nova Studio has not
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
