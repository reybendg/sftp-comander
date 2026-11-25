# SFTP Commander user guide

[Guía en español](guia-de-uso.md)

The interface is in Spanish; the original Spanish labels are shown in *italics* next to their meaning.

## The window

- **Connection bar** (top): saved servers drop-down (*Servidores*), host, port, user, password, *Conectar* (Connect) and *Administrar…* (Manage servers).
- **Tabs**: each tab is an independent session. The bar always shows the data of the visible tab.
- **Two panes**: local computer on the left, server on the right. The active pane has a blue border; actions apply to it.
- **Action buttons** and the **status bar** at the bottom. The status bar belongs to the tab, so a copy running in one tab does not hide the messages of another.

## Connecting

Type the server in the host field and press Enter or *Conectar*. The host field accepts:

- an IP or host name: `192.168.1.10`, `files.example.com`;
- an alias from `~/.ssh/config` (its user, port and key are used);
- `user@host:port` or `sftp://user@host:port`, which take priority over the separate fields.

An empty port means 22, or the port of the `~/.ssh/config` alias.

While connecting, a card over the remote pane shows each step and the elapsed time. *Cancelar* stops waiting immediately. After 15 seconds without an answer the attempt fails with an error.

### First connection to a server

The first time, a dialog shows the server's SHA256 fingerprint. If you trust it, it is saved to `~/.ssh/known_hosts`, the same file `ssh` uses. If a known server presents a **different** key, SFTP Commander refuses to connect. That can mean the server was reinstalled or someone is intercepting the connection. Check it and, if it is legitimate, remove the old line with `ssh-keygen -R host`.

### Authentication

It tries, in this order:

1. The private key of the saved server, or the keys from ssh-agent and `~/.ssh`.
2. The password typed in the bar.
3. The password stored in the keyring for this server, if any.

If the server still asks for a password, the *Contraseña* (Password) dialog opens. Tick *Recordar en el llavero del sistema* (Remember in the system keyring) to store it encrypted. Next time it connects without asking. If the stored password stops working, you are asked for a new one.

Passwords are never written to files. They live in the Secret Service keyring (GNOME Keyring on most desktops). You can see them in *Passwords and Keys* (Seahorse) as entries named "SFTP Commander: user@host:port", or delete them with *Olvidar contraseña* (Forget password) in the server manager.

## Saved servers

*Administrar…* (Ctrl+L) opens the list. Each server has:

| Field | Meaning |
|---|---|
| *Nombre* | How it appears in the list |
| *Servidor* | Host, IP or `~/.ssh/config` alias |
| *Puerto* | SSH port (optional) |
| *Usuario* | User (optional) |
| *Llave privada* | A specific private key (optional) |
| *Carpeta inicial* | Remote folder to open after connecting (optional) |
| *Servidor de salto* | Jump server to go through, `user@host:port` or an alias (optional) |
| *Llave del salto* | Private key for the jump server (optional; empty = ssh-agent and `~/.ssh` keys) |

*Guardar y conectar* (Save and connect) saves and connects. *Quitar* (Remove) deletes it from the list and also forgets its password. The list is stored in `~/.config/sftp-commander/servidores.ini` with permissions 600 and never contains passwords.

To connect, pick the server in the *Servidores* drop-down. If the visible tab is already connected, it opens in a new tab.

## Jump server (bastion)

When a server is only reachable through another one (a bastion), fill in *Servidor de salto* for that saved server. It works like `ssh -J`:

1. SFTP Commander connects to the jump server, checks its host key and signs in (with *Llave del salto*, or ssh-agent and your `~/.ssh` keys, or a password).
2. It opens a tunnel through the jump server to the destination. The destination name is resolved by the jump server, so internal names and addresses work.
3. It connects to the destination through that tunnel, with its own host key check and sign-in.

The connection card shows each step. The jump server only needs SSH, not SFTP.

If the jump server asks for a password, a separate dialog asks for it (*Contraseña del servidor de salto*). With *Recordar* ticked, it is stored in the keyring as "<server name> (salto)", separately from the destination's password. *Olvidar contraseña* and *Quitar* clear both.

The tab and the remote pane show the destination as `user@destination vía user@jump`. Lost connections and *Reconectar* work as with a direct connection: reconnecting goes through the jump server again.

From a terminal: `sftp-commander user@internal -J user@bastion:22`.

The password field of the connection bar is for the destination, not for the jump server.

## Warpgate and bastions with one-time codes or browser approval

[Warpgate](https://warpgate.null.page/) is not a jump server in the `ssh -J` sense: it is an SSH proxy you connect to directly, saying which target you want in the user name. Leave *Servidor de salto* empty and fill in:

| Field | Value |
|---|---|
| *Servidor* | the Warpgate host |
| *Puerto* | Warpgate's SSH port (2222 by default) |
| *Usuario* | `your_warpgate_user:target_name`, with a colon |

Or type it all in the host field: `david:web-server@warpgate.example.com:2222`.

Depending on your Warpgate user's policy:

- **Password**: asked in the usual password dialog; it can be remembered in the keyring.
- **Public key**: add your public key to your user in Warpgate's web interface.
- **One-time code (TOTP)**: after the password, a window asks for the code from your authenticator app.
- **Browser approval**: a window shows the approval link (also an *Abrir en el navegador* button) and the security key. Check that the key matches the one in the browser, approve there, then press *Continuar*.

The same window shows any other question a server asks during sign-in (keyboard-interactive). SFTP Commander never repeats an answer by itself: with a wrong code you get one failed attempt, and the server asks again or you reconnect. This matters because bastions block the account after several failures.

**First time with a target:** Warpgate wants you to confirm the target's host key, and it can only ask on a terminal. Over SFTP the connection is closed and SFTP Commander says so. Connect once with `ssh -p 2222 user:target@warpgate-host` in a terminal and answer `y`, or ask the Warpgate administrator to trust the key in its web interface. After that, SFTP works.

## Tabs

- **Ctrl+T** or the button at the right of the tab bar: new tab.
- **Ctrl+W** or the × on the tab: close it. A tab cannot be closed while it is copying (cancel first); if it is connecting, the attempt is cancelled.
- Ctrl+Page Down / Ctrl+Page Up switch tabs. Tabs can be reordered by dragging.

Each tab has its own connection, so you can copy in one tab while browsing another server in a second one.

### Lost connection

When you come back to a tab, SFTP Commander checks that its connection is still alive. If the network dropped or the server closed the connection, a dialog asks:

- *Reconectar* (Reconnect): connects again with the same data and goes back to the same remote folder.
- *Ahora no* (Not now): leaves the tab disconnected; use *Conectar* later.
- *Cerrar pestaña* (Close tab).

The same dialog appears if an operation fails because the connection is gone.

## Working with files

| Action | Keys | Notes |
|---|---|---|
| Copy to the other pane | F5 | Uploads or downloads; folders are copied recursively. Existing files are overwritten without asking. |
| Rename | F2 | |
| New folder | F7 | |
| New empty document | Shift+F4 | Never overwrites an existing file |
| Delete | F8 or Delete | Asks first; folders are deleted with all their content |
| Refresh | Ctrl+R | |
| Hidden files | Ctrl+H | |

The right-click menu has the same actions. Right-clicking an empty area clears the selection, which is handy for *New folder*.

Long copies show a progress bar and a *Cancelar* button in the status bar of that tab.

### Search

**Ctrl+F** (or *Buscar en esta carpeta* in the right-click menu) shows a search field in the active pane and filters the current folder as you type:

- it ignores upper and lower case: `log` finds `Servicio.LOG`;
- `*` and `?` work as wildcards: `*.conf`, `backup-2026-??-*`;
- Enter selects the first match; Esc clears the search;
- it is cleared when you change folder.

It only searches the current folder, not subfolders.

### Copy path

**Ctrl+Shift+C** (or *Copiar ruta*) copies the full path of the selected items to the clipboard, one per line. With nothing selected, it copies the path of the current folder.

### Open and edit remote files

Double-click a remote file (or press Enter or F4):

1. It is downloaded to `~/.cache/sftp-commander/editar/<random>/`.
2. It opens in a local app: for text, logs and configuration files, a graphical text editor; for images, an image viewer; otherwise the system default.
3. **Every time you save in that app, the file is uploaded back to the server**, and the status bar shows "*Guardado en el servidor*" (Saved on the server) with the time.
4. The temporary copies are deleted when SFTP Commander closes.

The upload goes through a tab connected to the same server. If none is connected, you are told to reconnect and save again.

To check which app would be used for a file: `sftp-commander --que-app file.txt`.

Limitation: if someone changes the file on the server while you edit it, your save overwrites their change.

## Files and locations

| What | Where |
|---|---|
| Saved servers | `~/.config/sftp-commander/servidores.ini` (permissions 600) |
| Passwords | System keyring (Secret Service), schema `io.github.sftpcommander.Clave` |
| Known servers | `~/.ssh/known_hosts` |
| Copies being edited | `~/.cache/sftp-commander/editar/` (deleted on exit) |
| Program after `make install` | `~/.local/bin/sftp-commander` |

On Windows:

| What | Where |
|---|---|
| Saved servers | `%LOCALAPPDATA%\sftp-commander\servidores.ini` |
| Passwords | Credential Manager, *Windows Credentials*, "SFTP Commander: user@host:port" |
| Keys, config and known hosts | `%USERPROFILE%\.ssh\` |
| Copies being edited | `%TEMP%\sftp-commander\editar\` (deleted on exit) |
| Switch drive | *Unidad* button in the local pane, or type `D:\` in the path |

## Troubleshooting

| Message or problem | What to do |
|---|---|
| *La llave del servidor cambió* (the server key changed) | The server presents a different key than the one stored. Check why. If it is legitimate: `ssh-keygen -R host` and connect again. |
| *El servidor pide contraseña* (the server asks for a password) | Your keys were not accepted. Type the password, or add your public key to the server's `~/.ssh/authorized_keys`. |
| *No se pudo guardar la contraseña en el llavero* (could not save to the keyring) | There is no Secret Service running (common on minimal desktops or over SSH). Install and start `gnome-keyring`, or keep typing the password. |
| *Este servidor no acepta contraseña* (this server does not accept passwords) | The server only accepts a key or its own questions (2FA code, approval). Leave the password empty. In Warpgate the web and SSH can have different rules: even if the web asks for your password, SSH may ask only for the code. To see what it accepts without spending an attempt: `ssh -o PreferredAuthentications=none -o PubkeyAuthentication=no -p 2222 user@host`. |
| *El servidor no ofrece SFTP* (no SFTP) | The SSH server has no SFTP subsystem. On OpenSSH, enable `Subsystem sftp` in `sshd_config`. |
| It takes long and then fails | The host is unreachable or a firewall drops the traffic. The attempt gives up after 15 seconds; *Cancelar* stops it earlier. |
| `sftp-commander: command not found` after `make install` | Add `~/.local/bin` to your `PATH` (most distributions do it after logging in again). |
