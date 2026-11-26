# Changelog

All notable changes to this project are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).

## [0.8.0] - 2026-11-26

### Added
- **About** window (F1 or the ☰ menu): version, author, link to the project and to Issues, and GTK/libssh versions for bug reports.
- **☰ menu** at the right of the connection bar, with *Ask before copying*, hidden files and About.
- **Drag and drop**: drag files from one pane to the other to copy them, or drop files from the system file manager (Explorer, Nautilus…) onto the server pane to upload them.
- **Sort by column**: click *Name*, *Size*, *Modified* or *Permissions* to sort; click again to reverse. Folders always stay on top.
- **Search button** (magnifier) next to each path, in addition to Ctrl+F.
- **Type to jump**: typing letters in a pane selects the first name that starts with them (or contains them); repeating the same letter goes to the next one, like Windows Explorer.
- Dragging one of several selected files carries the whole selection. Select several with Ctrl+click, Shift+click or Ctrl+A.
- **Confirmation before copying**, with *Don't ask again* (re-enable it in the ☰ menu). Stored in `~/.config/sftp-commander/preferencias.ini`.
- **Overwrite warning**: if something with the same name already exists in the destination, it always asks *Replace*, *Skip existing* or *Cancel* (Enter cancels).

### Changed
- **F5 refreshes** both panes, like a file manager (Ctrl+R still works). **F4 copies** to the other pane. Opening a file is Enter or double click.
- The Windows build includes the Spanish translations of GTK and finds its own icon.

## [0.7.0] - 2026-11-24

### Added
- Windows version (10 and 11, 64-bit), built with MSYS2 and shipped as a portable `.zip` (`make windows`, `tools/windows-portable.sh`). Everything works as on Linux: tabs, jump server, Warpgate, lost connection, search, editing remote files.
- Windows installer (`sftp-commander-<version>-instalador.exe`, NSIS): per-user install without administrator rights, Start menu entry, optional desktop shortcut and `sftp://` links, uninstaller in *Settings > Apps*.
- Own icon (`data/sftp-commander.svg`), used by the window, the `.exe`, the installer and the Linux menu entry (`.deb` and `make install`). `make icono` regenerates the PNG and `.ico` files.
- On Windows: passwords in the Credential Manager, a *Unidad* (drive) button in the local pane, keys and `known_hosts` from `%USERPROFILE%\.ssh`, remote files opened with the program associated with their extension (Notepad if none).

### Changed
- Remote paths are normalized with their own function, independent of the local operating system.

### Fixed
- When the server does not accept passwords at all (for example a Warpgate user whose SSH policy is key or one-time code only), the app said "wrong password" and asked for it again. Now the password is not sent, the app says the server does not accept passwords, and suggests a registered key or the server's questions instead. Tested with a test SSH server that offers no password method.

## [0.6.0] - 2026-11-24

### Added
- Keyboard-interactive questions from the server are shown in a window: one-time codes (TOTP), browser approval with a clickable link and an *Open in browser* button, or any other question. Tested against a real Warpgate bastion (password, password + TOTP, password + browser approval).
- `tools/probar-warpgate.sh`: automated test against Warpgate in Docker.
- `--prueba` asks the server's questions on the terminal.

### Changed
- Authentication tries each method once and follows "partial success" (for example key + code). The password is answered automatically only once.
- A clearer message when a bastion closes the SFTP channel because it still has to confirm the destination's host key on a terminal.

### Fixed
- The password was sent again as the answer to every server question, so with a one-time code the bastion recorded several failed logins and could block the account.

## [0.5.0] - 2026-11-24

### Added
- Jump server (bastion) support, like `ssh -J`: a saved server can have a *Servidor de salto* (jump host) and an optional key for it. The connection goes first to the jump host and then through an SSH tunnel (direct-tcpip channel) to the destination, whose name is resolved by the jump host, so internal names work.
- The jump host has its own host key check, its own password dialog and its own keyring entry ("<server> (salto)"). *Forget password* and *Remove* also clear it.
- `-J user@host[:port]` on the command line, also for `--prueba`.
- Works with any libssh from 0.8 on (it does not need libssh 0.11's native ProxyJump), so it behaves the same on Debian 13 and Ubuntu 24.04.

## [0.4.0] - 2026-11-19

### Added
- Tabs: one session per tab, each with its own connection, panes, status bar and progress. Ctrl+T opens a tab, Ctrl+W closes it. Choosing a saved server while the current tab is connected opens it in a new tab.
- Lost connection dialog: when a tab's connection dropped, coming back to it offers *Reconnect* (to the same remote folder), *Not now* or *Close tab*. Any remote operation that fails because the connection is gone shows it too.
- TCP keepalive and `TCP_USER_TIMEOUT` on the SSH socket, so a dead network is detected instead of hanging.
- Search in the current folder of a pane (Ctrl+F or right-click menu), case-insensitive, with `*` and `?` wildcards. Enter jumps to the first match, Esc clears it.
- Copy path (Ctrl+Shift+C or right-click menu): copies the full path of the selected items, or of the current folder.
- Public documentation in English and Spanish, this changelog and the GPL-3.0 license.
- `.deb` packages for Debian 13 and Ubuntu 24.04, built and tested in Docker with `make deb`, and a man page `sftp-commander(1)`.

### Changed
- The window title shows the server of the visible tab.

### Fixed
- Writing to a broken connection no longer kills the program with SIGPIPE.

## [0.3.1] - 2026-11-19

### Added
- "Connecting, please wait…" card with a spinner over the remote pane, showing the current step, the elapsed seconds and a Cancel button. While connecting, the Connect button turns into Cancel.

## [0.3.0] - 2026-11-19

### Added
- Password dialog when the server asks for a password that was not typed, or when it was wrong.
- Optional "Remember in the system keyring" (libsecret / GNOME Keyring) and a "Forget password" button in the server manager.
- Open and edit remote files (double click, Enter or F4): the file is downloaded to a temporary folder, opened with a local app and uploaded again every time it is saved. Text prefers a graphical text editor and images an image viewer.

## [0.2.0] - 2026-11-19

### Added
- Right-click menu in both panes.
- New empty document (Shift+F4); never overwrites an existing file.
- Saved servers: drop-down to connect and a manager window with name, host, port, user, private key and start folder.
- Separate host, port and user fields; custom SSH port.

## [0.1.0] - 2026-11-19

### Added
- First version: two panes (local and remote), host key verification with `~/.ssh/known_hosts`, authentication with keys, ssh-agent and password, recursive copy, delete, rename and new folder, with progress and cancel.

[0.8.0]: https://github.com/reybendg/sftp-comander/releases/tag/v0.8.0
[0.7.0]: https://github.com/reybendg/sftp-comander/releases/tag/v0.7.0
