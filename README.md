# HDE — Hyggshi Desktop Environment

A lightweight GTK3 desktop environment for X11 **and Wayland**: panel (Start menu, taskbar, system tray, status
area, notifications, clock, extensions), desktop icons and wallpaper, **Hyggshi Settings**, **Hyggshi Files** (the file
manager), a session manager and a system-hotkeys daemon.

## Features

| Area | What you get |
|------|--------------|
| **Start menu** | Three layouts (*Settings → Start Menu*): **Modern** like Linux Mint's Cinnamon menu — your picture and name, places (Home, Documents, Downloads, …), favorites and lock / log out / power on the left, a search box, the categories (they open under the mouse) and the apps with their descriptions on the right; **Kickoff** like KDE Plasma — search on top, favorites as tiles, *Applications* / *Places* tabs, Sleep / Restart / Shut Down; **Classic**, the small drop-down menu. **Press Super to open/close**, just type to search (names, descriptions, keywords, commands; accents optional), arrows / Tab / Enter / Esc, right-click (or the Menu key) an app: Add to Favorites, Pin to Panel, Add to Desktop. Recent files. Options: what it shows, icon size, menu size, the Start button's label and icon (the logo of your system — the default —, the HDE logo, ☰, an app grid, any icon of your theme or a picture of your own). **Right-click the Start button**: layout, icon and label right there |
| **Control Center** | Click the **network, Bluetooth or volume icon** or the bell (or `Super+A`): **quick toggles** — Wi-Fi, Bluetooth, Airplane mode, Do Not Disturb, Dark mode, Night Light, Power mode; the arrow of Wi-Fi opens the **network list right there** (a new secured network asks for its password in the list, saved / open ones connect at once), the arrow of Bluetooth the **paired devices** (connect / disconnect, their battery); a **brightness** slider (backlight, or software dimming) and a **volume** slider with mute whose arrow opens the **Sound** page: output devices, the microphone, input devices and **the volume of each app**; the latest **notifications** with their icons (click one to open it, × removes it, *Clear all*); the battery, screenshot, customize, settings, lock and power buttons. Esc goes back / closes. What it shows: *Settings → Panel → Control Center* |
| **Battery** | Click the battery icon: charge, charging or on battery and **the time left** (to full while charging), a **chart of the charge** over the last hours (UPower's history, or since login), the draw in watts, **the health of the battery** (full charge now vs. when new), cycles, voltage, temperature, model, the firmware's charge limit, the batteries of wireless mice / keyboards / headsets, and the **power mode** (Power Saver / Balanced / Performance, with power-profiles-daemon). From the kernel's own numbers: no upower needed. **Battery saver** (*Settings → Power*): when the battery runs low (10–50 %, or always on battery) the Power Saver mode, a dimmer screen and fewer background updates, all put back when you plug in. **Low-battery warnings** at 10 % and 5 % (that one stays until closed) |
| **Panel** | *Settings → Panel* (or **right-click the panel**: Panel Settings, Start Menu Settings, position, size, the items to show, extensions, Control Center, Task Manager, About), all live: **bottom or top**, height, transparency, which items it shows (Start button, Show Desktop, Run, pinned apps, taskbar with or without titles and grouping, workspaces, tray, status, notifications, clock), 12/24-hour clock with date and seconds. **Pinned apps** and **extensions**: processor and memory use (built in) or the output of any command refreshed every few seconds (weather, uptime, free disk space, … presets), with a command on click. Taskbar + workspaces (libwnck; on Wayland wlr-foreign-toplevel), system tray (XEmbed + StatusNotifierItem), Wi-Fi / Bluetooth / volume / battery status (right-click: a menu with icons for each), notification bell, calendar on the clock. **The panel measures the screen and itself** (straight from the X server, after every change): all of it always on the screen and the space it takes reserved, so maximized windows keep clear of it — moved or resized by someone else, it puts itself back; *Settings → Panel → Screen* shows the measurement (*Measure again*), `hde-panel --measure` prints it |
| **Notifications** | Built-in `org.freedesktop.Notifications` 1.2 daemon (actions, images, urgency, sounds, history) — `notify-send` and every app work |
| **Hotkeys** | Super = Start menu, **F1/F2/F3 = mute / volume down / volume up**, **F6/F7 = screen darker / brighter**, **F8 (or Super+P, or the display key of a laptop) = Project**, media + brightness keys with an on-screen display, screenshots, lock, terminal, files, run |
| **Screens (Project)** | **F8** opens a *Project* window like Windows + P: **PC screen only · Duplicate · Extend · Second screen only** for a projector, TV or second monitor (F8 again = next choice, Enter applies). Built on XRandR — no `xrandr`/`arandr` needed. *Second screen only* goes back by itself after 15 s unless kept (in case the other screen shows nothing). Plugging in a screen opens the window (or extends / duplicates, as chosen); unplugging the screen in use turns the computer's screen back on; the choice comes back at the next login for the same screens. The panel and desktop follow every change; each screen gets the whole wallpaper. **Resolution, refresh rate and orientation** of each screen in *Settings → Display* (the screens beside it move along; *Keep these display settings?* goes back by itself after 15 s), kept for the next logins |
| **Brightness** | **F6 / F7** and the brightness keys, with an OSD, **without brightnessctl**: the laptop backlight (directly or through systemd-logind); on desktop monitors **the monitor's own brightness over DDC/CI** when `ddcutil` is installed; else software dimming of every screen (virtual machines), kept for the next login. Slider in *Settings → Display* and the Control Center; *Night Light* (warmer colours) works too |
| **Screenshots** | Built-in `hde-screenshot` (no scrot or other tool needed): whole screen, drag an area, or the active window; saved to `~/Pictures/Screenshots`, copied to the clipboard, announced with a notification (Open / Show in Folder). Hold `Ctrl` to only copy to the clipboard. **Start menu → Screenshot** opens the *Screenshot* window: choose whole screen / window / area, a delay and whether the **mouse pointer** is in the picture, then Copy, Save As, Open or Show in Folder |
| **About** | *Settings → About* and the **About HDE** window (desktop menu): the **logo of your system** from `/etc/os-release` (`ID=` hyggshios, ubuntu, debian, linuxmint, … — the logo the system installs, else HDE's own copy of 27 distribution logos, else a badge), its name and version, **"Based on Debian 13 (trixie)" / "Ubuntu 24.04 LTS"** with that logo too, its links; HDE version and build, the computer (model, processor, RAM, graphics, storage, screens), and **how much RAM HDE uses right now** (per program, measured live); *Copy system info* for bug reports. `hde-settings --about` prints the same |
| **Wayland** | The **HDE (Wayland)** session on the login screen (needs `labwc`): the labwc compositor with HDE's panel, desktop, Start menu, notifications and OSD as layer-shell surfaces, a Wayland taskbar, the same keys (Super, F1–F3, F6–F8, PrtSc with `grim`, …) as labwc key bindings, title bars in HDE's colours; Settings for keyboard, touchpad and appearance apply there too |
| **Desktop** | Wallpaper + icons from `~/Desktop` with a clear selection frame (accent color) and hover highlight; **right-click an icon for its own menu** — Open, Open With, Open in Terminal, Cut, Copy, Rename, Move to Trash, Properties; rubber-band and Ctrl+click selection; Paste, Delete, F2 and the usual keyboard shortcuts |
| **Files** | **Hyggshi Files** (`hde-files`, in its own folder `hde-files/`), the file manager — and the one HDE uses: folders on the desktop, `Super+E`, *Show in Folder* of browsers and of the Screenshot tool open in it. Icons with **thumbnails** (pictures, and videos / PDF when a thumbnailer is installed) or a **list** (Name, Size, Type, Modified — click a column to sort), **tabs**, the places sidebar (bookmarks, drives, Trash, Recent), a path bar (`Ctrl+L` to type a path or an `sftp://` address, with completion), **search in the subfolders** (`Ctrl+F`), hidden files (`Ctrl+H`), zoom (`Ctrl` + wheel keys or the slider). **Copy, move, paste with progress and Cancel** — Replace / Skip / Keep Both / Merge when a name is taken, "name (copy)" in the same folder —, **drag and drop** (Ctrl copies, Shift moves), **the Trash** (restore where it was, delete, empty; no GVfs needed), **Undo** (`Ctrl+Z`), rename (the extension kept; `F2` once HDE's F1–F3 sound keys are off in *Settings → Keyboard & Shortcuts*, else from the menu), new folder / document (from `~/Templates`), Properties (size of folders, permissions, the app that opens the type), Open With, Run executables, compress / extract, open a terminal there; type the start of a name to jump to it; its Cut / Copy / Paste works with the desktop and the other file managers; it watches folders, so changes made elsewhere show at once; Light / Dark and the accent of HDE |
| **Network** | Real Wi-Fi list (NetworkManager): scan, signal, security, connect with password, disconnect, forget, hidden networks; wired/VPN devices |
| **Bluetooth** | Device list straight from BlueZ: paired + nearby devices, scan, pair (PIN/passkey/confirmation agent), connect, disconnect, remove |
| **Dark mode** | Applies immediately to the panel, menus, Settings and **every running GTK app** (`hde-xsettings`), GTK4/libadwaita via the `color-scheme` setting; picks the dark variant of your theme automatically. The **accent colour** (*Settings → Appearance*) is *Automatic* by default: the colour of your GTK theme (Yaru's orange, Adwaita's blue), so HDE's highlights match the switches and selections the theme draws; a colour chosen there also goes on those switches, sliders and selections |
| **Window managers** | GTK window managers **Metacity, Marco, Mutter, Muffin** (preferred — title bars follow the GTK theme and Dark mode), plus Xfwm4, Openbox, IceWM, Fluxbox and **NexWM** — **HDE's own window manager** (`nexwm/`: a real X11 window manager, EWMH/ICCCM over XCB, with frames, workspaces, snap / maximize / full screen, the struts of HDE's panel, and the key bindings of `~/.config/hde/nexwm.conf`). Switch live from Settings, no logout; the login screen also has a **NexWM** session (`/usr/share/xsessions/nexwm.desktop` = HDE started with `hde-start --wm nexwm`). See `nexwm/README.md` |
| **Session** | Restarts crashed components, XDG autostart (`~/.config/autostart`), polkit authentication agent, D-Bus activation environment |
| **Touchpad & mouse** | Scroll direction picked on the touchpad itself: *Like a phone* (the content follows your fingers, the default) or *Like a mouse wheel* (swipe up to go back toward the top), shown as pictures in *Settings → Input* and in a *Touchpad scrolling* window with a test page that opens once at the first login with a touchpad. Tap to click, separate mouse wheel direction, pointer speed/acceleration. A touchpad that X sees as a mouse (inside a virtual machine, or in PS/2 / HID mouse mode) is recognised on the test page ("It is my touchpad") and then follows the touchpad direction. Applied at login, immediately when changed, to devices plugged in later or back after suspend, and again whenever another program (a window manager with its own touchpad settings, a script) changes them (libinput, synaptics and evdev drivers, no `xinput` needed). GNOME's own touchpad / mouse settings (`org.gnome.desktop.peripherals`, and Cinnamon's) are kept the same, so Mutter and Muffin agree with HDE. Scrolling over the panel's volume icon follows the fingers (up = louder) whatever the direction. `hde-xsettings --status` shows every device and whether it matches Settings |
| **Settings that apply** | Sound (volume, mute, microphone, output device), keyboard layout + repeat, touchpad/mouse, screen timeout, text scale, wallpaper, icons, fonts, accent color, resolution, power mode, battery saver |
| **Power** | *Settings → Power*: the battery (charge, time left, health, cycles, model), the power mode, the battery saver and low-battery warnings, the screen timeout. Suspending after a while without use is left to a power manager (e.g. `xfce4-power-manager`, whose settings the page opens); the lid and the power button work through systemd-logind |

## Install

```sh
# build dependencies (libgtk-layer-shell-dev: for the Wayland session; libxcb1-dev: NexWM, HDE's own window manager)
sudo apt install build-essential pkg-config libgtk-3-dev libwnck-3-dev libxi-dev libxrandr-dev libgtk-layer-shell-dev \
                 libxcb1-dev
# recommended runtime packages
sudo apt install metacity network-manager bluez pipewire-pulse policykit-1-gnome \
                 gnome-themes-extra libnotify-bin playerctl
# for the HDE (Wayland) session: the compositor, screenshots, screen lock
sudo apt install labwc grim slurp swaylock
make
sudo make install          # PREFIX=/usr/local by default
```

Log out and choose the **HDE** session on the login screen — **HDE (Wayland)** (shown once `labwc` is installed) or **NexWM**, which is the same HDE session with HDE's own window manager (`nexwm/README.md`).
Logs: `~/.cache/hde/session.log`.
Updating an older copy (or one unpacked from a ZIP that still had prebuilt programs in `build/`): run
`make clean && make && sudo make install`, so that every program is rebuilt from the current sources.
**After updating HDE, log out and back in once** — `make dev` restarts the desktop, panel and hotkeys, but the order in
which the session starts things (hotkeys before the window manager, see below) only applies to a new login.
Without a display manager: `echo 'exec /usr/local/bin/hde-start' > ~/.xinitrc && startx`.

Try it in a window without logging out: `scripts/test-nexde` (a whole HDE session from `./build` in Xephyr, on its own
D-Bus, with a throw-away copy of your settings; `--fresh` = a new user, `--wm openbox`, `1920x1080 :3`, `--help`).
Needs `sudo apt install xserver-xephyr`.
Hyggshi OS: `packaging/hyggshi-os/build-nexwm.sh` builds and installs HDE inside the ISO's chroot.
Reload a running session after rebuilding: `make dev` (from `./build`) or `sudo make install && make reload`.

## Keyboard shortcuts

| Keys | Action |
|------|--------|
| `Super` (press and release), `Ctrl+Esc` | Open / close the Start menu — type to search (on Wayland with labwc older than 0.7.3: `Super+Space`) |
| `Super+S` | Search applications |
| `Super+A` | Control Center (quick toggles, Wi-Fi, Bluetooth, brightness, volume, notifications) |
| `Super+N` | The Control Center at its notifications |
| `Super+R`, `Alt+F2` | Run a command |
| `Super+E` | File manager (Hyggshi Files) |
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

In Hyggshi Files the same, plus: `Ctrl+N` / `Ctrl+T` / `Ctrl+W` new window / new tab / close the tab, `Alt+Left` /
`Alt+Right` / `Alt+Up` (or `Backspace`) back / forward / up, `Alt+Home` home, `Ctrl+L` type a location, `Ctrl+F`
search, `Ctrl+H` hidden files, `Ctrl+1` / `Ctrl+2` icons / list, `Ctrl++` / `Ctrl+-` / `Ctrl+0` zoom, `F9` sidebar,
`Ctrl+Shift+N` new folder, `Ctrl+Z` undo, `Ctrl+Shift+I` invert the selection, `Ctrl+D` bookmark, `Ctrl+Enter` open in a
new tab, `F5` reload, `F10` the menu, `Ctrl+?` all of them.

## Programs

| Program | Role |
|---------|------|
| `hde-session` | Session manager. `hde-session wm` switches the window manager live, `hde-session restart` restarts panel + desktop, `hde-session {logout,reboot,shutdown,suspend,lock}` |
| `nexwm` | HDE's own window manager (`nexwm/`, in this repository). `nexwm [--x11\|--wayland] [--replace] [--config FILE]`, `--version`, `--help`. The X11 side is a window manager of its own (XCB only, no libX11, no toolkit): frames, focus, workspaces, snap / maximize / full screen, `_NET_WM_STRUT` of the panel, `_NET_SUPPORTING_WM_CHECK`, `--replace` to take over from another window manager without logging out. Keys and settings in `~/.config/hde/nexwm.conf`; what the running one listens to is `xprop -root _NEXWM_KEYS`. The Wayland compositor of the same program is the next step: `nexwm --wayland` says what this build has. Both sides are optional at build time and `nexwm --version` names the ones this build got (a build without `libxcb` still compiles; `nexwm --x11` then says what to install) |
| `hde-panel` | Panel, Start menu, Control Center, battery panel, app search, notifications, OSD. `hde-panel --menu/--search/--run/--power/--show-desktop/--osd-volume/--osd-brightness N/--control-center[=wifi\|bluetooth\|sound\|notifications]/--notifications/--battery` control the running panel (X11 ClientMessage or D-Bus `org.hyggshi.HDE.Panel`); `hde-panel --measure` has the panel measure the screen again and put itself right, then prints the screen, the panel window, the space reserved for it and the room windows get (exit status 0 = it fits) |
| `hde-files` | Hyggshi Files, the file manager (`hde-files/`, which has its own Makefile too): `hde-files [FOLDER\|FILE\|URI…]` (a file: its folder with the file selected; `trash:///`, `recent:///`), `--select PATH…` (the folders with these items selected), `--new-window`, `--quit`. One process; while it runs it answers `org.freedesktop.FileManager1` (*Show in Folder*). In HDE sessions folders open in it (`hde-mimeapps.list`; a choice of your own in *Open With* wins). Its settings: `~/.config/hde/files.ini` |
| `hde-choose` | *Which program for this?* When more than one program on this machine can do the same thing — a terminal, a file manager, pictures, music and video, a system monitor, a screenshot tool — HDE asks which one to use, once, and remembers the answer. HDE's own program is offered first and marked *HDE's own* (Hyggshi Files, Hyggshi Media, hde-screenshot, and `hde-cmd` when the terminal of HDE is installed). `hde-choose terminal [ARGS…]` starts it (Super+T, Super+E and the other places of HDE run this), `--ask` asks even when the answer is remembered, `--no-ask` never asks, `--list` shows what is installed and what is remembered, `--set FEATURE PROGRAM` / `--reset FEATURE` change or forget an answer, and `HDE_CHOOSE=PROGRAM` overrides everything for one run. Where there is nothing to ask — one program installed, or no display — the first installed one runs and the log says why |
| `hde-desktop` | Wallpaper + desktop icons (icon menu, Cut/Copy/Paste compatible with GNOME/Xfce file managers) |
| `hde-settings` | Hyggshi Settings. `hde-settings <page>` opens a page; `hde-settings --style dark|light|toggle` switches Dark mode from a script; `--project` (the F8 window), `--display-mode pc\|duplicate\|extend\|second`, `--displays`, `--display-set NAME WxH[@HZ]\|auto [normal\|left\|right\|inverted]` (the resolution / rotation of a screen, kept for next time), `--brightness [+N\|-N\|N]`, `--power` (battery, battery saver), `--night-light [on\|off\|toggle]`, `--about` (this computer, the system, the RAM HDE uses), `--about-window`, `--wayland-config [DIR] [--reload]` (labwc's configuration for the Wayland session) |
| `hde-hotkeys` | System shortcuts (Xlib + XInput2). `hde-hotkeys --action NAME` does one action (volume-up, brightness-down, screenshot-area, project, lock, …): what the key bindings of the Wayland session run |
| `hde-screenshot` | Screenshot tool: `hde-screenshot [--area \| --window] [--delay N] [--file PATH \| --clipboard] [--pointer] [--no-clipboard] [--no-notify]`; `hde-screenshot --ui` = the Screenshot window |
| `hde-xsettings` | XSETTINGS manager: live theme / Dark mode / icons / fonts for all GTK apps; also HDE's input service: applies the touchpad / mouse settings (login, changes, hotplug, and back when another program changes them; keeps doing that when another XSETTINGS manager runs); and its display service: screens plugged in / unplugged, the layout chosen with F8 and the resolutions chosen in Settings at login, software brightness and Night Light after a screen change (and the software brightness of the last session at login). `--status`: devices vs Settings, screens, brightness method |

## Configuration

Everything lives in `~/.config/hde/settings.ini` (group `[settings]`), written by Hyggshi Settings:
`theme_index` (1 light, 2 dark), `gtk_theme`, `gtk_theme_effective`, `accent` (`auto` — the default: the colour of the
GTK theme — or `#rrggbb`), `icon_theme_name`, `font`,
`wm` (`auto`, `metacity`, `marco`, `mutter`, `muffin`, `xfwm4`, `openbox`, `icewm`, `fluxbox`, `nexwm`),
`super_menu`, `fkeys_sound`, `fkeys_display` (F6/F7/F8), `media_keys`, `system_shortcuts`, `screenshot_tool` (`builtin` or an installed
`gnome-screenshot`, `xfce4-screenshooter`, `mate-screenshot`, `flameshot`, `spectacle`, `maim`, `scrot`), `screenshot_pointer`
(the mouse pointer in screenshots, default `false`), `dnd`, `notification_popups`,
`notification_sounds`, `scale`, `keyboard_layout`, `repeat_rate`, `repeat_delay`, `screen_timeout`,
`natural_scroll` (touchpad: `true` = like a phone, the default; `false` = like a mouse wheel),
`touchpad_direction_chosen` (set once a direction was picked: the *Touchpad scrolling* window no longer opens at
login), `treat_as_touchpad` (names of touchpads that X sees as a mouse, e.g. inside a virtual machine: they follow
`natural_scroll`), `tap_to_click` (default `true`), `mouse_natural_scroll` (default `false`),
`pointer_speed` (0–1), `pointer_acceleration`, `display_connect` (when a screen is plugged in: `ask` = the F8 window,
`extend`, `duplicate`, `second`, `nothing`), `display_mode` + `display_outputs` (the last F8 choice and for which
screens), `display_modes` (resolution / rotation per screen: `HDMI-1=1920x1080@74.97/normal,eDP-1=1280x800@59.91/left`),
`night_light`, `night_light_temperature` (kelvin, default 4000), `battery_saver` (default `false`), `battery_saver_level`
(10, 15, 20 — the default —, 30, 50 %, or 100 = always on battery), `battery_saver_dim` (default `true`),
`battery_warnings` (default `true`), …
The software brightness of the last session (no backlight, no DDC/CI) is kept in `~/.local/state/hde/state.ini`.
Panel: `panel_position` (`bottom`/`top`), `panel_size`, `panel_opacity`, `panel_show_menu` / `_desktop` / `_run` /
`_launchers` / `_taskbar` / `_workspaces` / `_tray` / `_status` / `_notifications` / `_clock`, `panel_taskbar_labels`,
`panel_taskbar_group`, `clock_24h`, `clock_show_date`, `clock_show_seconds`, `panel_launchers` (pinned apps),
`panel_applets` + one `[applet:ID]` group each (`type=cpu|memory|command|separator`, `label`, `command`, `interval`,
`click`). Start menu: `menu_style` (`modern`, `kickoff`, `classic`), `menu_show_sidebar` / `_places` / `_favorites` /
`_recent` / `_descriptions`, `menu_hover_switch`, `menu_icon_size`, `menu_size`, `menu_favorites`, `menu_button_label`,
`menu_button_icon` (`os` — the default —, `menu`, `hde`, `none`, an icon name or the path of a picture). Control Center:
`cc_status_click` (the status icons open it; `false`: the old actions), `cc_wifi` / `cc_bluetooth` / `cc_airplane` /
`cc_dnd` / `cc_dark` / `cc_night_light` / `cc_power_mode` / `cc_brightness` / `cc_volume` / `cc_notifications`.
`choice_terminal`, `choice_files`, `choice_monitor`, `choice_pictures`, `choice_player`, `choice_screenshot`: the
program hde-choose uses for each of them (a program name, or `ask` to ask every time; written by the question itself
and by `hde-choose --set`, forgotten by `--reset`).
All documented in `src/hde-panel-config.h`.
Desktop wallpaper and icon positions: `~/.config/hde/config.ini`. The Wayland session's labwc configuration is
written to `~/.config/hde/labwc/` from these settings (rewritten when they change).

## Tests

`make check` starts a complete HDE session inside Xvfb and checks the Super key, F1–F3 volume
(PulseAudio), notifications, PrtSc screenshots (also with an Openbox `rc.xml` that binds `Print`, and with Openbox
holding `Print` before `hde-hotkeys` starts), `Ctrl+Print` to the clipboard, the desktop
icon selection frame and icon menu (Rename, Trash, Copy/Paste, Properties), the Wi-Fi list (with a simulated
`nmcli`), live Dark mode, the Control Center (tiles, the brightness and volume sliders, a Wi-Fi password typed in the
list, the Bluetooth devices of a simulated BlueZ, output devices and apps of PulseAudio, notifications), the battery
panel with a fake battery (`HDE_POWER_SUPPLY_DIR`) and a simulated power-profiles-daemon, the battery saver and the
low-battery warnings on a draining fake battery, *Settings → Power*, the mouse pointer in screenshots, the software
brightness kept for the next login, GNOME's touchpad settings following HDE's, the right-click menus of the
panel, the Start button and the volume icon, maximized windows below a top panel and above a bottom one (Metacity and
Openbox), the panel measuring itself and putting itself back when moved or resized, *Settings → Panel → Screen*, the
sidebar of Settings without a black frame, the accent colour following the GTK theme (Yaru), Hyggshi Files (opening a
folder, thumbnails, hidden files, icons / list, new folder, type-ahead and rename, copy / paste / undo, the Trash and
Restore, the right-click menus, Properties, search in subfolders, tabs, *Show in Folder* over D-Bus, `--select`, Dark
mode, a folder on the desktop opening in it), live window manager switching and crash recovery; `power-test` checks the
battery numbers (µWh / µAh, time left, health, two batteries, a mouse) and the battery saver's decisions without X;
`measure-test` where the panel belongs and what is wrong when it is not there (one or two screens, scale 2).
`tests/display-test.sh` (real Xorg with several dummy screens) also checks resolutions (Settings and
`--display-set`, the screens beside moving along, kept or reverted, back at login) and DDC/CI with a simulated `ddcutil`;
`scripts/test-nexde --check` starts a nested session in Xephyr; `packaging/hyggshi-os/build-nexwm.sh` runs in a Debian 13
container and the installed session is started. Needs `xvfb xdotool dbus-x11` (optionally `metacity openbox
pulseaudio libnotify-bin imagemagick xinput`). CI runs it on Ubuntu 22.04 and 24.04 and also builds HDE on Debian 13
(trixie, GCC 14) and Debian testing (newest GCC, C23 by default).

`tests/display-test.sh` (CI only: needs root) runs a whole session on Xorg with the dummy video driver, whose RandR
outputs act as real connectors: F8 / Super+P, Extend / Duplicate / Second screen only (with the automatic way back) /
PC screen only checked with `xrandr`, the panel and desktop following, F6/F7 software dimming and Night Light (gamma
ramps), the layout restored at login, a screen plugged in opening the Project window, a (fake) laptop backlight, and
the RAM the desktop uses. `tests/randr-plan-test.c` checks the layouts themselves without an X server.

`tests/nexwm-test.sh` runs the window manager for real in Xvfb: NexWM takes the screen over and publishes what a
session needs (`_NET_SUPPORTING_WM_CHECK`, `_NET_CLIENT_LIST`, work area, the bindings as `_NEXWM_KEYS`), frames a
window with the border from its configuration file, follows its keys (workspaces, move-to, snap, maximize, full screen,
spawn, close as a polite `WM_DELETE_WINDOW`), keeps a maximized window clear of a panel's struts, gives every window
back when it quits, and takes over from a running Metacity with `--replace`; `tests/nexwm-test.c` checks the
configuration file and the key bindings without any display (`make check-unit`). Both run with `make check-nexwm`,
which says so and stops when this build has no X11 side (no `libxcb`).

`tests/choose-test.c` (plain C, no display, `make check-unit`) checks the rules of *which program for this?*: the
features, what is installed on a PATH the test makes up, two names for one program counted once
(`x-terminal-emulator`), and the answers in settings.ini. `tests/choose-run-test.sh` runs the program itself — what it
starts and what it says, one program installed vs several, nothing installed (exit status 127 and what to install), the
caller's arguments passed on, the arguments of the table used only when there are none (`hde-choose files` → the home
folder), what it remembers (`--set`, `--reset`, a remembered program that is gone, `HDE_CHOOSE`) — and, through the
stand-in for GTK3 of `tests/choose-stub/` (the same trick as the stand-in for mpv of the player test), the question
itself: the line that was clicked is the one that starts, and only *Remember my choice* writes it down.

`tests/wayland-test.sh` runs the HDE (Wayland) session for real — labwc with its headless backend (no screen or GPU),
started by `hde-start --wayland` — and checks the panel and desktop as layer-shell surfaces, the Start menu through
D-Bus, the Super key and Ctrl+Esc as labwc key bindings, typing in the menu, the taskbar with a real window, Show
Desktop, the volume key, PrtSc with `grim`, notifications, Settings and About on Wayland, the panel moved to the top,
labwc reloading its configuration when a setting changes, and logging out. CI runs it on Debian 13. Needs
`labwc grim wtype dbus zenity imagemagick`.

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
- `hde-files/`: Hyggshi Files, the file manager (`src/files.h` describes its parts).
- `apps/hde-session.c`: session manager. `src/hde-wm.h`: window-manager table shared with Settings.
- `src/hde-choose.h` / `src/hde-choose.c` (the table of *which program for this?*, what is installed, what was
  remembered) and `apps/hde-choose.c` (the question, the running of the program); `tests/choose-stub/` is a stand-in
  for GTK3, built only for `tests/choose-run-test.sh`.
- `nexwm/`: NexWM, HDE's own window manager (`src/nexwm.h`, `src/config.c` — the configuration file, `src/x11.c` — the
  X11 window manager, `src/wayland.c` — the Wayland compositor, `src/nexwm.c` — the program itself).
- `hde-core/`: backend-neutral APIs and core services; `backend/x11/`, `backend/wayland/`: session actions.
- Panel IPC (`src/hde-ipc.h`): root property `_HDE_PANEL_WINDOW` + ClientMessage `_HDE_PANEL_COMMAND`; D-Bus
  `org.hyggshi.HDE.Panel` (`Command(i command, u time, i argument)`) on both X11 and Wayland.
- Wayland: `src/hde-wl.c` (layer-shell helpers, no-ops without gtk-layer-shell), `src/hde-wltaskbar.c`
  (wlr-foreign-toplevel-management, `protocols/`), `src/hde-settings-wayland.c` (labwc's configuration),
  `hde-session --wayland` (starts labwc, which starts `hde-session --wayland-inner`).
- Start menu `src/hde-startmenu.c`, panel settings `src/hde-panel-config.c`, extensions `src/hde-applets.c`,
  logos of the systems `src/hde-osinfo.c` + `src/hde-svgpath.c` + `data/logos/`.

New features target `hde-core` APIs first; X11/Wayland-specific operations belong in `backend/*`.
