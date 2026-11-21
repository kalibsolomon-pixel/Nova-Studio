# Installing Nova Studio

Download the installer or portable ZIP and its checksum from the GitHub release. The Windows
packages are unsigned; verify the downloaded file against the release's `SHA256SUMS.txt`.

## Check the download

Compare the SHA-256 checksum of the file you downloaded with the published `SHA256SUMS.txt`.
In PowerShell:

```powershell
Get-FileHash .\nova-studio-0.1.3-windows-x64-mingw.exe -Algorithm SHA256
```

## Install

1. Run `nova-studio-0.1.3-windows-x64-mingw.exe`.
2. The installer is not code-signed. If Windows SmartScreen appears, choose **More info**, confirm
   the file name and checksum, then **Run anyway**. Do not disable SmartScreen or other Windows
   security features.
3. Keep the suggested folder, `%LOCALAPPDATA%\Programs\Nova Studio`, or choose an **empty** folder
   whose path is at most 160 characters long. Setup refuses folders that already contain other
   files.
4. Finish, and start Nova Studio from the Start menu.

Setup installs for your Windows account only. It needs no administrator rights, adds no service,
driver, scheduled task, file association or PATH entry, and does not change any AI agent's
settings.

Silent install (for scripting): `nova-studio-0.1.3-windows-x64-mingw.exe /S`
(add `/D=C:\Path\To\Folder` as the last argument to choose the folder).

## Upgrade or reinstall

Run a newer (or the same) installer. It installs into the existing folder. It first removes the
previous version's files with that version's own uninstaller, then copies the new files. Close
Nova Studio and any AI agent connected to it first; Setup waits and asks you to retry if they are
still running. AI agents keep working after an upgrade because nova-mcp.exe stays in the same place.

Only one installation per Windows user is supported. To move it, uninstall first.

## Updates

A few seconds after it starts, Nova Studio asks GitHub for the latest release of
`kalibsolomon-pixel/Nova-Studio`. When a newer version exists, Nova downloads its installer to
`%LOCALAPPDATA%\Nova Studio\updates` and checks it against the release's `SHA256SUMS.txt`.
Nothing is installed until you choose **Install and Restart**: Nova then closes (asking about
unsaved work first), the installer upgrades the installation without showing its pages and starts
Nova Studio again. **Later** asks again at the next start; **Skip This Version** waits for the
version after it. If the installation does not finish, Nova says so at the next start and offers
to run the installer with its pages, which show the reason.

**Help › Check for Updates…** checks at any time. Turn off the startup check in
**Edit › Preferences › Startup**. The portable ZIP and builds from source only show the release
page. The checksum comes from the same release as the installer, so it detects a damaged download,
not a compromised release; the installer is not code-signed.

## Uninstall

Settings › Apps › Installed apps › **Nova Studio** › Uninstall, or run
`Uninstall Nova Studio.exe` in the installation folder.

Uninstalling removes only the files Setup installed, the Start menu shortcut and the uninstall
entry. It keeps:

- your resource packs, wherever they are;
- settings, recovery snapshots and diagnostics in `%LOCALAPPDATA%\Nova Studio`;
- the Minecraft test runtime and test world in `%LOCALAPPDATA%\Nova Studio`;
- files you added to the installation folder yourself.

**AI agents:** agents you set up keep an entry named `nova-studio` that points to the removed
nova-mcp.exe. Remove it from the agent (for example `claude mcp remove nova-studio --scope user`,
`codex mcp remove nova-studio`, or delete it from `claude_desktop_config.json`), or the agent will
report that the server failed to start.

To remove all Nova Studio data as well, delete `%LOCALAPPDATA%\Nova Studio` after uninstalling.

## Portable ZIP

`nova-studio-0.1.3-windows-x64-mingw.zip` contains the same files. Extract it to a folder you
can write to and run `nova-studio.exe`. Use it for troubleshooting or where installing is not
wanted. It has no Start menu entry or uninstaller; delete the folder to remove it.

## If an installation was interrupted

Run the installer again with the same folder. It recognizes its own unfinished installation,
removes what was copied and installs again. Or run `Uninstall Nova Studio.exe` from that folder.
