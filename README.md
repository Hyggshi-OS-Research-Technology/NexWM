# HDE — Hyggshi Desktop Environment

A lightweight GTK3 / X11 desktop environment: panel (Start menu, taskbar, system tray, status
area, notifications, clock), desktop icons and wallpaper, **Hyggshi Settings**, a session
manager and a system-hotkeys daemon.

## Features

| Area | What you get |
|------|--------------|
| **Start menu** | Categories, custom launchers (`~/.config/hde/start-apps/*.desktop`), **press Super to open/close**, just start typing to search apps (or `Super+S`) |
| **Panel** | Taskbar + workspaces (libwnck), system tray (XEmbed + StatusNotifierItem), Wi-Fi / Bluetooth / volume / battery status, notification bell with Do Not Disturb, calendar on the clock |
| **Notifications** | Built-in `org.freedesktop.Notifications` 1.2 daemon (actions, images, urgency, sounds, history) — `notify-send` and every app work |
| **Hotkeys** | Super = Start menu, **F1/F2/F3 = mute / volume down / volume up**, media + brightness keys with an on-screen display, screenshots, lock, terminal, files, run |
| **Screenshots** | Built-in `hde-screenshot` (no scrot or other tool needed): whole screen, drag an area, or the active window; saved to `~/Pictures/Screenshots`, copied to the clipboard, announced with a notification (Open / Show in Folder) |
| **Desktop** | Wallpaper + icons from `~/Desktop` with a clear selection frame (accent color) and hover highlight; **right-click an icon for its own menu** — Open, Open With, Open in Terminal, Cut, Copy, Rename, Move to Trash, Properties; rubber-band and Ctrl+click selection; Paste, Delete, F2 and the usual keyboard shortcuts |
| **Network** | Real Wi-Fi list (NetworkManager): scan, signal, security, connect with password, disconnect, forget, hidden networks; wired/VPN devices |
| **Bluetooth** | Device list straight from BlueZ: paired + nearby devices, scan, pair (PIN/passkey/confirmation agent), connect, disconnect, remove |
| **Dark mode** | Applies immediately to the panel, menus, Settings and **every running GTK app** (`hde-xsettings`), GTK4/libadwaita via the `color-scheme` setting; picks the dark variant of your theme automatically |
| **Window managers** | GTK window managers **Metacity, Marco, Mutter, Muffin** (preferred — title bars follow the GTK theme and Dark mode), plus Xfwm4, Openbox, IceWM, Fluxbox, NexWM. Switch live from Settings, no logout |
| **Session** | Restarts crashed components, XDG autostart (`~/.config/autostart`), polkit authentication agent, D-Bus activation environment |
| **Settings that apply** | Sound (volume, mute, microphone, output device), keyboard layout + repeat, touchpad/mouse (libinput via xinput), screen timeout, text scale, wallpaper, icons, fonts, accent color |

## Install

```sh
# build dependencies
sudo apt install build-essential pkg-config libgtk-3-dev libwnck-3-dev libxi-dev
# recommended runtime packages
sudo apt install metacity network-manager bluez pipewire-pulse policykit-1-gnome \
                 gnome-themes-extra libnotify-bin brightnessctl playerctl
make
sudo make install          # PREFIX=/usr/local by default
```

Log out and choose the **HDE** session on the login screen. Logs: `~/.cache/hde/session.log`.
Without a display manager: `echo 'exec /usr/local/bin/hde-start' > ~/.xinitrc && startx`.

Try it nested: `Xephyr :2 -screen 1280x720 & DISPLAY=:2 ./build/hde-session`.
Reload a running session after rebuilding: `make dev` (from `./build`) or `sudo make install && make reload`.

## Keyboard shortcuts

| Keys | Action |
|------|--------|
| `Super` (press and release) | Open / close the Start menu — type to search |
| `Super+S` | Search applications |
| `Super+R`, `Alt+F2` | Run a command |
| `Super+E` | File manager |
| `Super+D` | Show desktop |
| `Super+L` | Lock screen |
| `Ctrl+Alt+T` | Terminal |
| `Ctrl+Alt+Delete` | Session / Power dialog |
| `F1` / `F2` / `F3` | Mute / volume down / volume up (turn off in *Settings → Keyboard & Shortcuts*) |
| Volume, mic-mute, brightness, play/pause keys | Work out of the box, with an OSD |
| `Print`, `Shift+Print`, `Alt+Print` | Screenshot: screen / area (drag; `Esc` cancels) / active window |

`hde-session` starts `hde-hotkeys` **before** the window manager, so these keys stay HDE's even when the window
manager's own configuration binds them too (for example an Openbox `rc.xml` with `Print` → `scrot`, which used to
end in *Failed to execute child process "scrot"*). If a shortcut still does nothing, see the session log.

On the desktop: `Enter` open, `Alt+Enter` properties, `F2` rename, `Delete` move to Trash (`Shift+Delete` delete),
`Ctrl+A` / `Ctrl+C` / `Ctrl+X` / `Ctrl+V` select all / copy / cut / paste, `Menu` or `Shift+F10` context menu.

## Programs

| Program | Role |
|---------|------|
| `hde-session` | Session manager. `hde-session wm` switches the window manager live, `hde-session restart` restarts panel + desktop, `hde-session {logout,reboot,shutdown,suspend,lock}` |
| `hde-panel` | Panel, Start menu, app search, notifications, OSD. `hde-panel --menu/--search/--run/--power/--osd-volume` control the running panel |
| `hde-desktop` | Wallpaper + desktop icons (icon menu, Cut/Copy/Paste compatible with GNOME/Xfce file managers) |
| `hde-settings` | Hyggshi Settings. `hde-settings <page>` opens a page; `hde-settings --style dark|light|toggle` switches Dark mode from a script |
| `hde-hotkeys` | System shortcuts (Xlib + XInput2) |
| `hde-screenshot` | Screenshot tool: `hde-screenshot [--area \| --window] [--delay N] [--file PATH] [--no-clipboard] [--no-notify]` |
| `hde-xsettings` | XSETTINGS manager: live theme / Dark mode / icons / fonts for all GTK apps |

## Configuration

Everything lives in `~/.config/hde/settings.ini` (group `[settings]`), written by Hyggshi Settings:
`theme_index` (1 light, 2 dark), `gtk_theme`, `gtk_theme_effective`, `accent`, `icon_theme_name`, `font`,
`wm` (`auto`, `metacity`, `marco`, `mutter`, `muffin`, `xfwm4`, `openbox`, `icewm`, `fluxbox`, `nexwm`),
`super_menu`, `fkeys_sound`, `media_keys`, `system_shortcuts`, `screenshot_tool` (`builtin` or an installed
`gnome-screenshot`, `xfce4-screenshooter`, `mate-screenshot`, `flameshot`, `spectacle`, `maim`, `scrot`), `dnd`, `notification_popups`,
`notification_sounds`, `scale`, `keyboard_layout`, `repeat_rate`, `repeat_delay`, `screen_timeout`, …
Desktop wallpaper and icon positions: `~/.config/hde/config.ini`.

## Tests

`make check` starts a complete HDE session inside Xvfb and checks the Super key, F1–F3 volume
(PulseAudio), notifications, PrtSc screenshots (also with an Openbox `rc.xml` that binds `Print`), the desktop
icon selection frame and icon menu (Rename, Trash, Copy/Paste, Properties), the Wi-Fi list (with a simulated
`nmcli`), live Dark mode, live window manager switching and crash recovery. Needs `xvfb xdotool dbus-x11` (optionally `metacity openbox
pulseaudio libnotify-bin imagemagick`). CI runs it on Ubuntu 22.04 and 24.04.

## Architecture

- `src/`: the GTK3 desktop programs (panel, desktop, settings pages, hotkeys, xsettings).
- `apps/hde-session.c`: session manager. `src/hde-wm.h`: window-manager table shared with Settings.
- `hde-core/`: backend-neutral APIs and core services; `backend/x11/`: X11 implementation;
  `backend/wayland/`: placeholder for the next phase.
- Panel IPC (`src/hde-ipc.h`): root property `_HDE_PANEL_WINDOW` + ClientMessage `_HDE_PANEL_COMMAND`.

New features target `hde-core` APIs first; X11/Wayland-specific operations belong in `backend/*`.
