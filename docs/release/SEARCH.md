# Search (Ctrl+K)

Search finds commands, tools, settings, workspaces, panels and textures without knowing which menu
they are in.

- Open it with **Ctrl+K**, **Ctrl+Shift+P**, **Help › Search** or **View › Search…**, from any
  workspace.
- Type part of a name, a related word or a shortcut. Matching ignores case: `export animation`,
  `minecraft`, `layers`, `opacity`, `uv`, `undo`, `mcp`, `appearance`, `ctrl+z`, `f8`.
- **Up/Down** and **Page Up/Page Down** move through results, **Enter** runs the selected result,
  **Esc** closes Search.

| Result | What Enter does |
| --- | --- |
| Command | Runs the same command as the menu item. |
| Tool | Selects the tool. |
| Workspace | Switches workspace. The active one is marked *(current)*. |
| Panel | Shows the panel and brings it to the front. It never hides a panel. |
| Setting | Opens Preferences (or the panel that holds the control) with that setting selected. |
| Asset | Opens the texture or model. |

Commands that cannot run right now stay in the list, marked **Unavailable**, with the reason below
the list, for example "Open a texture to use this command."

Search reads the menus as they are when you open it, and textures from the project index Nova Studio
already keeps, so it never scans your disk while you type.
