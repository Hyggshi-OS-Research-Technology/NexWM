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
| **Hotkeys** | Super = Start menu, **F1/F2/F3 = mute / volume down / volume up**, **F6/F7 = screen darker / brighter**, **F8 (or Super+P, or the display key of a laptop) = Project**, media + brightness keys with an on-screen display, screenshots, lock, terminal, files, run |
| **Screens (Project)** | **F8** opens a *Project* window like Windows + P: **PC screen only · Duplicate · Extend · Second screen only** for a projector, TV or second monitor (F8 again = next choice, Enter applies). Built on XRandR — no `xrandr`/`arandr` needed. *Second screen only* goes back by itself after 15 s unless kept (in case the other screen shows nothing). Plugging in a screen opens the window (or extends / duplicates, as chosen); unplugging the screen in use turns the computer's screen back on; the choice comes back at the next login for the same screens. The panel and desktop follow every change; each screen gets the whole wallpaper |
| **Brightness** | **F6 / F7** and the brightness keys, with an OSD, **without brightnessctl**: the laptop backlight (directly or through systemd-logind), or software dimming of every screen on desktop monitors and virtual machines (which have no backlight a program can change). Slider in *Settings → Display*; *Night Light* (warmer colours) works too |
| **Screenshots** | Built-in `hde-screenshot` (no scrot or other tool needed): whole screen, drag an area, or the active window; saved to `~/Pictures/Screenshots`, copied to the clipboard, announced with a notification (Open / Show in Folder). Hold `Ctrl` to only copy to the clipboard. **Start menu → Screenshot** opens the *Screenshot* window: choose whole screen / window / area and a delay, then Copy, Save As, Open or Show in Folder |
| **About** | *Settings → About*: HDE version and build, the computer (model, processor, RAM, graphics, storage, screens), the system, and **how much RAM HDE uses right now** (per program, measured live); *Copy system info* for bug reports. `hde-settings --about` prints the same |
| **Desktop** | Wallpaper + icons from `~/Desktop` with a clear selection frame (accent color) and hover highlight; **right-click an icon for its own menu** — Open, Open With, Open in Terminal, Cut, Copy, Rename, Move to Trash, Properties; rubber-band and Ctrl+click selection; Paste, Delete, F2 and the usual keyboard shortcuts |
| **Network** | Real Wi-Fi list (NetworkManager): scan, signal, security, connect with password, disconnect, forget, hidden networks; wired/VPN devices |
| **Bluetooth** | Device list straight from BlueZ: paired + nearby devices, scan, pair (PIN/passkey/confirmation agent), connect, disconnect, remove |
| **Dark mode** | Applies immediately to the panel, menus, Settings and **every running GTK app** (`hde-xsettings`), GTK4/libadwaita via the `color-scheme` setting; picks the dark variant of your theme automatically |
| **Window managers** | GTK window managers **Metacity, Marco, Mutter, Muffin** (preferred — title bars follow the GTK theme and Dark mode), plus Xfwm4, Openbox, IceWM, Fluxbox, NexWM. Switch live from Settings, no logout |
| **Session** | Restarts crashed components, XDG autostart (`~/.config/autostart`), polkit authentication agent, D-Bus activation environment |
| **Touchpad & mouse** | Scroll direction picked on the touchpad itself: *Like a phone* (the content follows your fingers, the default) or *Like a mouse wheel* (swipe up to go back toward the top), shown as pictures in *Settings → Input* and in a *Touchpad scrolling* window with a test page that opens once at the first login with a touchpad. Tap to click, separate mouse wheel direction, pointer speed/acceleration. A touchpad that X sees as a mouse (inside a virtual machine, or in PS/2 / HID mouse mode) is recognised on the test page ("It is my touchpad") and then follows the touchpad direction. Applied at login, immediately when changed, to devices plugged in later or back after suspend, and again whenever another program (a window manager with its own touchpad settings, a script) changes them (libinput, synaptics and evdev drivers, no `xinput` needed). Scrolling over the panel's volume icon follows the fingers (up = louder) whatever the direction. `hde-xsettings --status` shows every device and whether it matches Settings |
| **Settings that apply** | Sound (volume, mute, microphone, output device), keyboard layout + repeat, touchpad/mouse, screen timeout, text scale, wallpaper, icons, fonts, accent color |

## Install

```sh
# build dependencies
sudo apt install build-essential pkg-config libgtk-3-dev libwnck-3-dev libxi-dev libxrandr-dev
# recommended runtime packages
sudo apt install metacity network-manager bluez pipewire-pulse policykit-1-gnome \
                 gnome-themes-extra libnotify-bin playerctl
make
sudo make install          # PREFIX=/usr/local by default
```

Log out and choose the **HDE** session on the login screen. Logs: `~/.cache/hde/session.log`.
Updating an older copy (or one unpacked from a ZIP that still had prebuilt programs in `build/`): run
`make clean && make && sudo make install`, so that every program is rebuilt from the current sources.
**After updating HDE, log out and back in once** — `make dev` restarts the desktop, panel and hotkeys, but the order in
which the session starts things (hotkeys before the window manager, see below) only applies to a new login.
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
| `F6` / `F7` | Screen darker / brighter, with an OSD (turn off with the F6–F8 switch in *Settings → Keyboard & Shortcuts*) |
| `F8`, `Super+P`, the display key | *Project*: PC screen only / Duplicate / Extend / Second screen only. Press again for the next one, `Enter` applies, `1`–`4` apply directly, `Esc` closes |
| Volume, mic-mute, brightness, display, play/pause keys | Work out of the box, with an OSD |
| `Print`, `Shift+Print`, `Alt+Print` | Screenshot: screen / area (drag; `Esc` cancels) / active window |
| `Ctrl+Print`, `Ctrl+Shift+Print`, `Ctrl+Alt+Print` | The same, copied to the clipboard only (no file) |

`hde-session` starts `hde-hotkeys` **before** the window manager, so these keys stay HDE's even when the window
manager's own configuration binds them too (for example an Openbox `rc.xml` with `Print` → `scrot`, which used to
end in *Failed to execute child process "scrot"*). Every `Print` combination is HDE's, so no such binding is left over.
If another program got `Print` first anyway — typically a session that an older HDE started (window manager first) —
a notification says so; HDE takes the key over as soon as that program lets go of it (for example when the window
manager is switched in Settings), and logging out and back in fixes it for good. Details are in the session log.

On the desktop: `Enter` open, `Alt+Enter` properties, `F2` rename, `Delete` move to Trash (`Shift+Delete` delete),
`Ctrl+A` / `Ctrl+C` / `Ctrl+X` / `Ctrl+V` select all / copy / cut / paste, `Menu` or `Shift+F10` context menu.

## Programs

| Program | Role |
|---------|------|
| `hde-session` | Session manager. `hde-session wm` switches the window manager live, `hde-session restart` restarts panel + desktop, `hde-session {logout,reboot,shutdown,suspend,lock}` |
| `hde-panel` | Panel, Start menu, app search, notifications, OSD. `hde-panel --menu/--search/--run/--power/--osd-volume` control the running panel |
| `hde-desktop` | Wallpaper + desktop icons (icon menu, Cut/Copy/Paste compatible with GNOME/Xfce file managers) |
| `hde-settings` | Hyggshi Settings. `hde-settings <page>` opens a page; `hde-settings --style dark|light|toggle` switches Dark mode from a script; `--project` (the F8 window), `--display-mode pc\|duplicate\|extend\|second`, `--displays`, `--brightness [+N\|-N\|N]`, `--about` (this computer, the system, the RAM HDE uses) |
| `hde-hotkeys` | System shortcuts (Xlib + XInput2) |
| `hde-screenshot` | Screenshot tool: `hde-screenshot [--area \| --window] [--delay N] [--file PATH \| --clipboard] [--no-clipboard] [--no-notify]`; `hde-screenshot --ui` = the Screenshot window |
| `hde-xsettings` | XSETTINGS manager: live theme / Dark mode / icons / fonts for all GTK apps; also HDE's input service: applies the touchpad / mouse settings (login, changes, hotplug, and back when another program changes them; keeps doing that when another XSETTINGS manager runs); and its display service: screens plugged in / unplugged, the layout chosen with F8 at login, software brightness and Night Light after a screen change. `--status`: devices vs Settings, screens, brightness method |

## Configuration

Everything lives in `~/.config/hde/settings.ini` (group `[settings]`), written by Hyggshi Settings:
`theme_index` (1 light, 2 dark), `gtk_theme`, `gtk_theme_effective`, `accent`, `icon_theme_name`, `font`,
`wm` (`auto`, `metacity`, `marco`, `mutter`, `muffin`, `xfwm4`, `openbox`, `icewm`, `fluxbox`, `nexwm`),
`super_menu`, `fkeys_sound`, `fkeys_display` (F6/F7/F8), `media_keys`, `system_shortcuts`, `screenshot_tool` (`builtin` or an installed
`gnome-screenshot`, `xfce4-screenshooter`, `mate-screenshot`, `flameshot`, `spectacle`, `maim`, `scrot`), `dnd`, `notification_popups`,
`notification_sounds`, `scale`, `keyboard_layout`, `repeat_rate`, `repeat_delay`, `screen_timeout`,
`natural_scroll` (touchpad: `true` = like a phone, the default; `false` = like a mouse wheel),
`touchpad_direction_chosen` (set once a direction was picked: the *Touchpad scrolling* window no longer opens at
login), `treat_as_touchpad` (names of touchpads that X sees as a mouse, e.g. inside a virtual machine: they follow
`natural_scroll`), `tap_to_click` (default `true`), `mouse_natural_scroll` (default `false`),
`pointer_speed` (0–1), `pointer_acceleration`, `display_connect` (when a screen is plugged in: `ask` = the F8 window,
`extend`, `duplicate`, `second`, `nothing`), `display_mode` + `display_outputs` (the last F8 choice and for which
screens), `night_light`, `night_light_temperature` (kelvin, default 4000), …
Desktop wallpaper and icon positions: `~/.config/hde/config.ini`.

## Tests

`make check` starts a complete HDE session inside Xvfb and checks the Super key, F1–F3 volume
(PulseAudio), notifications, PrtSc screenshots (also with an Openbox `rc.xml` that binds `Print`, and with Openbox
holding `Print` before `hde-hotkeys` starts), `Ctrl+Print` to the clipboard, the desktop
icon selection frame and icon menu (Rename, Trash, Copy/Paste, Properties), the Wi-Fi list (with a simulated
`nmcli`), live Dark mode, live window manager switching and crash recovery. Needs `xvfb xdotool dbus-x11` (optionally `metacity openbox
pulseaudio libnotify-bin imagemagick xinput`). CI runs it on Ubuntu 22.04 and 24.04 and also builds HDE on Debian 13
(trixie, GCC 14) and Debian testing (newest GCC, C23 by default).

`tests/display-test.sh` (CI only: needs root) runs a whole session on Xorg with the dummy video driver, whose RandR
outputs act as real connectors: F8 / Super+P, Extend / Duplicate / Second screen only (with the automatic way back) /
PC screen only checked with `xrandr`, the panel and desktop following, F6/F7 software dimming and Night Light (gamma
ramps), the layout restored at login, a screen plugged in opening the Project window, a (fake) laptop backlight, and
the RAM the desktop uses. `tests/randr-plan-test.c` checks the layouts themselves without an X server.

`tests/input-test.sh` (CI only: needs root) checks the touchpad settings against the real X input drivers: Xorg with
the libinput, synaptics and evdev drivers and virtual touchpads / mice created through `/dev/uinput`. It checks natural
scrolling and tap to click on a fresh account, live changes, hotplug and remove/re-add (suspend/resume), changes made
by another program, another XSETTINGS manager, `hde-xsettings --status`, that a two-finger swipe up really moves the
content up, and — in the *Touchpad scrolling* window — that a real swipe up over the test page goes toward the end
with *Like a phone* and back toward the top with *Like a mouse wheel*; that a mouse scrolling the test page makes the
window ask *Is it your touchpad?* and that the touchpad direction then applies to it.

Which build is running: *Settings → About → Build*, `hde-settings --version`, `hde-xsettings --version` and the first
line of `~/.cache/hde/session.log` show the commit HDE was built from.

## Architecture

- `src/`: the GTK3 desktop programs (panel, desktop, settings pages, hotkeys, xsettings).
- `apps/hde-session.c`: session manager. `src/hde-wm.h`: window-manager table shared with Settings.
- `hde-core/`: backend-neutral APIs and core services; `backend/x11/`: X11 implementation;
  `backend/wayland/`: placeholder for the next phase.
- Panel IPC (`src/hde-ipc.h`): root property `_HDE_PANEL_WINDOW` + ClientMessage `_HDE_PANEL_COMMAND`.

New features target `hde-core` APIs first; X11/Wayland-specific operations belong in `backend/*`.
