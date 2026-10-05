# HDE session black-screen fix

The old `hde-session` initialized the backend, printed one line, shut the backend down and exited. That caused the Xephyr session to become a black/empty screen.

This version keeps the session alive, resolves `hde-desktop` and `hde-panel` relative to the `hde-session` executable (then PATH and standard bin directories), starts them, and cleans them up on SIGTERM/SIGINT.

## Test with the existing NexDE binaries

Build the migration tree:

    make

`make` builds `build/hde-session`, `build/hde-desktop`, `build/hde-panel` and the other programs from source
(the repository contains no prebuilt programs; see fix 7).

Run:

    Xephyr :2 -screen 1280x720 &
    DISPLAY=:2 ./build/hde-session

If `hde-session` is installed beside the desktop/panel binaries, it will launch them automatically.

Optional:

    DISPLAY=:2 ./build/hde-session --no-panel
    DISPLAY=:2 ./build/hde-session --no-desktop

## "Icons / Start menu cannot be clicked" bug (fix 2)

The real causes when running `DISPLAY=:2 ./build/hde-session` in Xephyr:

1. `resolve_component()` used a *static buffer*: `hde-desktop` and `hde-panel` pointed to the same string ->
   the session ran **two panels and no desktop** (log: `starting desktop: .../hde-panel`).
2. The host runs Wayland (`WAYLAND_DISPLAY` is set), so the backend picked `wayland` and GTK opened its windows
   on the host instead of inside Xephyr. Now: if `DISPLAY` is set, X11 is used and the session sets `GDK_BACKEND=x11`.
3. Old binaries in `/usr/local/bin` (with Vietnamese log messages) were used instead of the new build. `make` now builds
   `hde-desktop`, `hde-panel`, `hde-settings` from `src/` into `build/`; the session looks next to itself first.
4. The session did not start a window manager -> added (xfwm4, openbox, ...); disable with `--no-wm` or `HDE_NO_WM=1`.

Run again:

    make clean && make
    Xephyr :2 -screen 1280x720 &
    DISPLAY=:2 ./build/hde-session

## Works in Xephyr but does not start on a real machine (fix 3)

On a real machine the session is started via `hde.desktop` -> `hde-start` -> `hde-session`, but previously
`make install` only installed `hde-session`: it did not install `hde.desktop` (xsessions) or `hde-start`
(which was not executable either), and `@PREFIX@` was never substituted -> the login screen
had no valid HDE session, or the session exited immediately.

Installation:

    sudo apt install libgtk-3-dev libwnck-3-dev openbox   # openbox or xfwm4: window manager
    make clean && make
    sudo make install            # PREFIX=/usr/local by default

Then log out and pick the **HDE** session on the login screen (gear icon), an X11 session.
If the session does not come up, check `~/.cache/hde/session.log` (and `~/.xsession-errors`).

No display manager: `echo 'exec /usr/local/bin/hde-start' > ~/.xinitrc && startx`

## Mouse / input clicks do not work (fix 4)

1. **Nested in Xephyr on a Wayland host**: GTK3 uses XInput2 and therefore gets no real mouse clicks
   from Xephyr (xdotool/XTEST still work, which makes it hard to notice). `hde-session` and `hde-start` now set
   `GDK_CORE_DEVICE_EVENTS=1` automatically when `WAYLAND_DISPLAY` or `HDE_CORE_EVENTS=1` is set.
   Disable with `HDE_XI2=1`. Manual test: `GDK_CORE_DEVICE_EVENTS=1 DISPLAY=:2 ./build/hde-session`.
2. **Run dialog**: Enter did not run the command (no default response) -> fixed, Enter = Run.
3. `hde-start` sets the root cursor to `left_ptr` (if `xsetroot` is available) so the pointer is always visible in a real session.

Run again:

    make clean && make
    Xephyr :2 -screen 1280x720 &
    DISPLAY=:2 ./build/hde-session


## HDE real-machine additions

- `hde-hotkeys`: X11 system shortcut daemon.
  - `PrtSc/SysRq`: full-screen screenshot.
  - `Shift+PrtSc`: area screenshot.
  - `Alt+PrtSc`: active-window screenshot.
  - Uses HDE's own `hde-screenshot` (no external tool needed); Settings → Keyboard & Shortcuts can pick an
    installed `gnome-screenshot`, `xfce4-screenshooter`, `flameshot`, `scrot`, … instead (fix 6).
- `hde-session` starts the window manager before GTK desktop components (and, since fix 6, after `hde-hotkeys`).
  - `HDE_WM=auto` uses the fallback list.
  - `HDE_WM=xfwm4`, `openbox`, `marco`, etc. selects a specific WM.
- Panel status shows input method/Fcitx, Wi-Fi and Bluetooth state from the real machine.
- Settings adds Bluetooth and Window Management pages.
- `settings.ini` can contain `wm=xfwm4` (or `auto`) and `hde-start` passes it to the session.

## Wi-Fi, Bluetooth, Super key, F1–F3, GTK WMs, Dark mode and the essential DE components (fix 5)

### Fixed / added

| Request | Before | Now |
|---------|--------|-----|
| **Wi-Fi list** | The Network page only had `nmcli device status` rows + a Wi-Fi switch | Real network list (grouped by SSID, signal, security, connected / saved), **Scan**, connect (asks for the password — fed to `nmcli --ask` through stdin, never visible in `ps`), disconnect, forget, hidden networks, enterprise networks (802.1X) open the advanced editor |
| **Bluetooth shows no list** | Only "adapter detected" + a switch | Reads BlueZ directly over D-Bus: **My devices** (paired: connect/disconnect/remove, battery) + **Other devices** (45-second scan, pairing with an agent that asks for a PIN/passkey/code confirmation), on/off, visibility to other devices, automatic `rfkill unblock`, a clear message when bluetoothd is not running |
| **Super opens the Start menu** | Missing | Press and release **Super** = open/close the menu (XInput2 raw events; the key is not grabbed, so `Super+key` shortcuts of the WM/apps keep working). Typing while the menu is open = app search |
| **F1, F2, F3 = sound** | Missing | F1 mute/unmute, F2 down, F3 up (max 100%), with an OSD; media/brightness keys work too. Turn F1–F3 off in *Settings → Keyboard & Shortcuts* to give the keys back to applications |
| **GTK WMs instead of Openbox/xfwm** | List xfwm4 → openbox → … | Supports **Metacity, Marco, Mutter, Muffin** (preferred in Auto mode — window borders follow the GTK theme and Dark mode), switch WM **instantly without logging out** (*Settings → Window Management → Apply now* or `hde-session wm`) |
| **Dark mode in Settings that really works** | The "Dark" combo only stored a number and did nothing | Applies immediately to the panel, menus, Settings and **every open GTK application** (`hde-xsettings` daemon), writes `~/.config/gtk-3.0/settings.ini` and GSettings `color-scheme` (GTK4/libadwaita), finds the dark variant of the theme automatically (creates Adwaita-dark when `gnome-themes-extra` is missing). From the command line: `hde-settings --style dark` |

### "Must-have" DE components added

- **Notification daemon** (`org.freedesktop.Notifications`) in the panel: popups, action buttons, images, sounds, history under the bell button, **Do Not Disturb**.
- **Polkit agent**: started by hde-session (polkit-gnome / mate / lxpolkit / kde…) so applications that need admin rights can ask for a password.
- **XDG autostart**: runs the entries in `~/.config/autostart` and `/etc/xdg/autostart` (honoring `OnlyShowIn`/`NotShowIn`/`Hidden`/`TryExec`).
- **Automatic restart after a crash** (panel, desktop, hotkeys, xsettings, WM), rate-limited.
- Volume/brightness **OSD**, a **calendar** when clicking the clock, an **app search** box, screen locking with a real locker (light-locker, xscreensaver, dm-tool, i3lock…).
- `dbus-update-activation-environment` so that D-Bus services (portal, keyring…) know the DISPLAY of the session.
- Settings that really apply: volume/mic/output device, keyboard layout + key repeat rate, touchpad (see fix 8), screen blanking, font size, wallpaper (Settings used to save the wallpaper to the wrong file, so the desktop never changed).
- Build fix: `hde-core/include/hde/core.h` and `hde-core/integration/core.c` were hidden by `.gitignore` (`core.*`), so the repo did not build.

### Install and try

    sudo apt install build-essential pkg-config libgtk-3-dev libwnck-3-dev libxi-dev
    sudo apt install metacity network-manager bluez pipewire-pulse policykit-1-gnome libnotify-bin
    make clean && make
    sudo make install          # then log out and pick the HDE session

Quick try without installing: `Xephyr :2 -screen 1280x720 & DISPLAY=:2 ./build/hde-session`.
Automated tests (Xvfb): `sudo apt install xvfb xdotool dbus-x11 && make check`.

`make check` runs a whole HDE session in Xvfb and verifies 51 checks (CI runs on Ubuntu 22.04 and 24.04):
the Super key opens/closes the menu (also while another application is in use and right after a Super+key combo), typing searches for apps,
F1/F2/F3 really change the volume through PulseAudio (capped at 100%; with F1–F3 turned off the keys go back to applications), notifications,
the Wi-Fi list + connecting with a password (the password goes through stdin, a wrong password asks again, cancelling leaves no broken profile),
the Bluetooth list + pairing/connecting through a mock BlueZ, live Dark mode via XSETTINGS, switching WM without logging out,
automatic panel restart after a crash and a clean logout. Diagnostic logging: `HDE_DEBUG=1`.

Note: when nested in Xephyr on a Wayland host, the nested X server may not deliver XInput2 raw events;
the Super key then does not open the menu (click the Menu button or use `hde-panel --menu`); a real session works normally.

## Desktop icon menu, selection frame and built-in screenshots (fix 6)

### Fixed / added

- **`Failed to execute child process "scrot"` on PrtSc.** HDE's own code never ran scrot unchecked; the dialog
  came from the window manager: Openbox (and LXDE-style `rc.xml` files) bind `Print` → `scrot` and `W-e` →
  `kfmclient`, and Openbox reports a missing program in exactly that dialog. The WM used to start before
  `hde-hotkeys`, grabbed those keys first, and HDE's grab failed. Now:
  - `hde-session` starts `hde-hotkeys` first and waits until it reports (through `HDE_READY_FD`) that its keys are
    grabbed; only then does it start the window manager. An X key can only be grabbed by one program, so
    PrtSc, Super+E, … stay HDE's whatever the WM config says.
  - `hde-hotkeys` re-grabs without ever releasing a key it keeps (a keyboard layout change made the WM and HDE
    re-grab at the same moment).
  - New built-in tool `hde-screenshot` (GTK3): whole screen, area (drag on a frozen, dimmed copy of the screen;
    `Esc`/right-click cancels), active window (with its title bar, without CSD shadows). Saves
    `~/Pictures/Screenshots/Screenshot_<date>_<time>.png`, copies the picture to the clipboard and shows a
    notification with *Open* / *Show in Folder*. Also in the app menu as *Screenshot*.
  - `screenshot_tool=` (Settings → Keyboard & Shortcuts) can choose an installed external tool; a chosen tool
    that is missing falls back to the built-in one instead of failing.
- **Desktop icons get their own context menu.** Right-click on an icon used to fall through to the desktop
  menu. Now it opens the icon's menu: Open, Open With (applications for that file type + *Other Application…*),
  Open in Terminal (folders), Cut, Copy, Rename…, Move to Trash, Properties. On an icon that is part of a
  multi-selection the menu acts on all selected icons; Home only offers Open, Open With, Open in Terminal and
  Properties. Cut/Copy use the GNOME/Xfce file-manager clipboard format, Paste copies folders recursively,
  moves after Cut and never overwrites (`name (copy).ext`). Rename keeps the icon's saved position; renaming a
  launcher changes its displayed name. `Type=Link` launchers now show their name/icon and open their URL.
- **Visible selection frame.** Icons are `GtkEventBox`es without their own window, which never paint CSS
  backgrounds or borders, so the `.selected`/`:hover` styles were invisible. The frame is now drawn with cairo:
  accent-colored fill, light edge and a dark outline (visible on light and dark wallpapers), plus a hover
  highlight; cut items are shown faded.

### Tests

`make check` now also checks: hde-hotkeys starts before the WM; Print saves a full-screen PNG, notifies and
fills the clipboard; Shift+Print shows the overlay and `Esc` cancels; a dragged 200×150 area is saved exactly;
`screenshot_tool=scrot` without scrot falls back; Alt+Print captures only the active window; with Openbox and an
`rc.xml` that binds `Print`, Print still takes HDE's screenshot; the selection frame and hover highlight are
visible (pixel colors); each icon menu (file, folder, Home) has the right items and the desktop menu still opens
on empty space; Properties, Rename, Move to Trash, Copy + Paste of a folder and the Delete key work; no
"Failed to execute child process" appears in the session log.

## Print is HDE's in every situation, Ctrl+Print to the clipboard (fix 7)

The fixes of fix 6 only take effect in a session that the new `hde-session` started: it starts `hde-hotkeys` before
the window manager. In a session that was still started by an older `hde-session` (window manager first — for
example after `make dev` or `sudo make install` without logging out), Openbox keeps `Print` and its `rc.xml`
binding (`scrot`) still runs. Now:

- `hde-hotkeys` notices when another program holds one of its keys and shows a notification *"Print key taken by
  another program"* that names the window manager and says what to do (log out and back in). The session log lists
  every key that is taken.
- It takes such keys over by itself as soon as they are free: it retries every 2 seconds and, when the window
  manager exits or is replaced (switching the window manager in Settings, a crash), every 10 ms for 2 seconds, so a
  newly started window manager cannot grab them again.
- **Ctrl+Print**, **Ctrl+Shift+Print** and **Ctrl+Alt+Print** copy a screenshot (screen / area / window) to the
  clipboard only, without saving a file (`hde-screenshot --clipboard`; the notification shows a preview). With
  these, every Print combination is HDE's, so WM bindings like `C-Print` → `scrot -s` cannot run either.
- Without `notify-send` (libnotify-bin), `hde-hotkeys` sends its notifications with `gdbus`.
- **Stale prebuilt programs removed from the repository.** `build/hde-desktop`, `hde-panel`, `hde-session` and
  `hde-settings` were committed by accident (`build/` is in `.gitignore`) and dated from before all of these fixes.
  `make` decides by file dates, so whenever those files were not older than the sources — a GitHub *Download ZIP*
  or tarball (every file gets the same date), `git checkout -- build`/`git stash`/`git reset --hard` after a
  build (the old files come back with a new date) — `make` printed nothing to do for them and `sudo make install`
  installed the OLD desktop, panel and settings: no icon menu, no selection frame, none of the new pages. Now
  `make` always builds every program. With an older copy, run `make clean && make && sudo make install` once.
- CI also builds everything on Debian 13 (trixie, GCC 14) and Debian testing (newest GCC, C23 by default).

`make check` additionally checks: the clipboard really offers `image/png` after Print and Ctrl+Print; Ctrl+Print
saves no file and shows a notification with a preview; with Openbox holding Print before `hde-hotkeys` starts,
HDE reports it (log + notification), and after the window manager is replaced Print takes HDE's screenshot again
(the new Openbox, whose `rc.xml` binds Print, does not get it).

## Touchpad scrolls the wrong way (fix 8)

Swiping up on the touchpad moved the content down, and swiping down moved it up. *Settings → Input* showed
**Natural scrolling: on** ("scroll content in the same direction as your fingers"), but nothing applied it: Settings
only applied values that had been saved in `settings.ini`, i.e. switches the user had flipped. On a fresh account the
touchpad therefore kept the X driver's default — classic scrolling (the content moves against the fingers) and no
tap to click — the opposite of what the page showed. It also needed the `xinput` program, did nothing for touchpads
driven by the synaptics driver, and nothing re-applied the settings to a device plugged in later or re-added by the
kernel after suspend/resume.

Now (`src/hde-input.c`):

- The touchpad does what Settings shows: **natural scrolling** (swipe up = the content moves up, as on a phone or a
  Windows precision touchpad) and **tap to click** are applied on every login, also when they were never changed.
  To get the classic direction back, turn *Natural scrolling* off in *Settings → Input → Touchpad*.
- Applied directly through the X server (XInput 2 device properties): no `xinput` needed. Works with the libinput
  driver (`libinput Natural Scrolling Enabled`, `libinput Tapping Enabled`, speed, acceleration profile) and the
  synaptics driver (negative `Synaptics Scrolling Distance`, `Synaptics Tap Action`).
- `hde-xsettings` applies them at login, as soon as `settings.ini` changes, and to every pointer device that is added
  or enabled later (USB/Bluetooth mice and touchpads, a touchpad re-added after suspend/resume); once more a few
  seconds later in case another program (Mutter, an autostart script) set its own values at the same moment.
  `pkill -HUP hde-xsettings` re-applies everything. The session log lists every touchpad/mouse and its state.
- *Settings → Input* applies a switch immediately, has its own **mouse** wheel direction (*Mouse → Natural
  scrolling*, off by default) and lists the devices with their current state (driver, natural scrolling, tap to click).
- Mouse direction, pointer speed and acceleration are only applied once changed in Settings, so a system-wide
  `xorg.conf` setting keeps working until then.
- Settings: every page now opens at its top. All pages share one scrolled area and used to keep the scroll position
  of the previous page, so after scrolling e.g. *Keyboard & Shortcuts*, the Input page opened scrolled down with the
  touchpad switches out of sight (Appearance showed only "Wallpaper").

### Tests

`tests/input-test.sh` (CI job *touchpad*, Ubuntu 22.04 and 24.04) runs a real Xorg server (dummy video driver) with
the libinput and synaptics input drivers and virtual devices created through `/dev/uinput`: a clickpad, a second
touchpad plugged in later, a wheel mouse and a synaptics touchpad. It records the driver defaults (classic
direction — the reported bug) and then checks: natural scrolling and tap to click right after login on a fresh
account without `xinput` in `PATH`; a real two-finger swipe UP moves the content UP (the window receives scroll-down
events) and DOWN moves it down; the mouse wheel is untouched; hotplug and remove/re-add; every switch live (classic
direction, tap to click, mouse natural scrolling, speed, acceleration); SIGHUP; `hde-settings --apply`; the device
list in *Settings → Input*. `make check` checks the same logic in Xvfb with a simulated libinput touchpad, including
a click on the *Natural scrolling* switch.
