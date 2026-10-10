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
  To get the classic direction back, choose *Like a mouse wheel* in *Settings → Input → Touchpad* (fix 9; it used to
  be a *Natural scrolling* switch).
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

## Touchpad still scrolls "the wrong way": pick the direction on the touchpad itself (fix 9)

After fix 8 the touchpad scrolled like a phone (natural scrolling) by default, but "swipe up and it goes down" is
read both ways: *it* can be the content (then natural scrolling is the cure) or the page position (then it is the
problem). Neither direction is right for everyone — phones, Macs and Windows precision touchpads move the content
with the fingers; a mouse wheel and the classic Linux setting go the other way — and the old switch label could not
make that clear. On top of that, some setups changed the direction back behind HDE's back.

Now:

- **The choice is shown, not described**: *Settings → Input → Touchpad → Scroll direction* has two picture cards,
  **Like a phone** ("the page follows your fingers: swipe up to read on") and **Like a mouse wheel** ("swipe up to go
  back toward the top of the page"). Each picture shows two fingers moving up and which way the page then moves.
- **Try both before deciding**: the **Touchpad scrolling** window (*Try both…* on that page, or
  `hde-settings --touchpad-setup`) has a test page with numbered lines; scrolling it with two fingers says where you
  went ("from line 46 to line 70: toward the end of the page"), and a card click changes the touchpad at once.
- **Asked once, on the machine itself**: at the first login with a touchpad, hde-session opens that window by itself
  (`hde-settings --touchpad-setup=auto`, 2 s after login). Once the user closes it, `touchpad_direction_chosen=true`
  is saved and it does not open again.
- **Nothing changes it back any more**: `hde-xsettings` now watches the touchpad properties (`XI_PropertyEvent`).
  When another program sets a different value — a window manager with its own touchpad settings (Mutter, Muffin, which
  the *Auto* window manager setting prefers), an autostart script, `xinput` — it puts Settings' value back within a
  third of a second (logged as `changed by another program, set back`). A program that keeps fighting is left alone
  for a minute instead of looping. `settings.ini` is read first, so a change made in Settings is never undone.
- **Also inside another desktop**: when another XSETTINGS manager already runs (xfsettingsd, gsd-xsettings,
  xsettingsd) or takes over later, `hde-xsettings` no longer exits — it leaves the theme settings to it and keeps
  applying the touchpad and mouse settings. A second `hde-xsettings` still exits at once (`--replace` restarts it).
- **Easy to check**: `hde-xsettings --status` lists every pointer device with its current state and whether it matches
  Settings (exit status 1 if not), says whether HDE's input service runs, and names devices HDE cannot change (no
  libinput/synaptics driver). *Settings → Input* warns when the input service is not running in the session.
- **Which build runs**: *Settings → About → Build*, `--version` and the first line of the session log show the commit
  HDE was built from (from git, or from `data/version` in a GitHub ZIP download), so an old installed copy is easy to
  spot.

### Tests

`tests/input-test.sh` (real Xorg, libinput + synaptics drivers, uinput devices) additionally checks: a value changed
by another program is set back without a signal or re-login (libinput and synaptics); `--status` reports a differing
device (exit 1) and a matching set (exit 0); a second `hde-xsettings` exits; the *Touchpad scrolling* window opens at
the first login with a touchpad, a **real two-finger swipe up over its test page goes toward the end with *Like a
phone* and back toward the top with *Like a mouse wheel***, the choice is applied at once (also to the synaptics
touchpad) and not undone by `hde-xsettings`, *Done* remembers it and the window does not open again; with another
XSETTINGS manager running, `hde-xsettings` stays and keeps applying Settings. `make check` (Xvfb) clicks the cards on
the Input page, opens the window with *Try both…*, scrolls its test page, and checks the first-login window and that
it stays closed once a direction was chosen.

## Touchpad that X sees as a mouse, evdev, and the volume icon (fix 10)

Fixes 8 and 9 change the scroll direction of *touchpads*. Some touchpads never reach X as one, so the choice changed
nothing for them and "swipe up and it goes down" stayed:

- **Inside a virtual machine** (VirtualBox, VMware, QEMU/KVM, Hyper-V) the host turns two-finger swipes into wheel
  turns of a virtual mouse ("VirtualBox mouse integration", "ImExPS/2 Generic Explorer Mouse", "QEMU USB Tablet").
- **Touchpads in mouse mode**: a touchpad running as a PS/2 mouse, or an I2C touchpad whose multi-touch part is not
  used ("ELAN… Mouse", "SYNA… Mouse"): the touchpad itself turns the swipes into wheel turns.
- **The evdev X driver** (when xserver-xorg-input-libinput is not installed) was not handled at all.

Now:

- **The test page knows which device scrolled it.** In the *Touchpad scrolling* window (*Settings → Input → Try
  both…*, or opened at the first login), scrolling the test page with a device that X sees as a mouse shows
  *"You scrolled with “…”, which HDE sees as a mouse — usual inside a virtual machine, or for a touchpad in mouse mode —
  so the choice above does not reach it. Is it your touchpad?"* with an **It is my touchpad** button. From then on
  that device follows the touchpad direction (*Like a phone* / *Like a mouse wheel*), applied at once, on every login,
  after hotplug and when another program changes it — like a real touchpad. Saved as `treat_as_touchpad` in
  `settings.ini`.
- **Settings → Input → Devices** has an **It is a touchpad** check box next to every mouse (unticking it gives the
  device the mouse wheel direction back) and a note when no touchpad was found at all.
- **First login on a virtual machine or a laptop where only a mouse was found**: the *Touchpad scrolling* window now
  opens too (before, it opened only when a touchpad was found), with a note that the touchpad may arrive as a mouse.
- **evdev driver**: the scroll direction is applied with `Evdev Scrolling Distance` (negative = natural scrolling),
  for the mouse wheel setting and for devices marked as the touchpad.
- **Volume icon**: scrolling over the panel's volume icon follows the fingers (or the wheel) physically — up = louder.
  With natural scrolling the X driver turns a swipe up into "scroll down" so that pages follow the fingers, and that
  used to turn the volume *down*. The panel now reads the scrolling device's direction and turns it around.
- `hde-xsettings --status` and the session log name such devices "mouse used as the touchpad" and list evdev devices.

### Tests

`tests/input-test.sh` (CI job *touchpad*) now also runs the evdev driver: a mouse pinned to evdev keeps its wheel
direction by default, gets natural scrolling (negative scrolling distance — the window then receives scroll-down for
wheel up) and back, and is listed by `--status`. With the libinput wheel mouse playing a touchpad seen as a mouse: its
wheel over the test page makes the window ask *Is it your touchpad?*; *It is my touchpad* saves it, applies *Like a
phone* at once (wheel up now reads on toward the end of the page), *Like a mouse wheel* and back apply to it too,
`--status` shows "mouse used as the touchpad … OK", `hde-xsettings` sets it back when another program changes it, and
unticking *It is a touchpad* in *Settings → Input → Devices* gives it the mouse direction back. With the touchpads
unplugged, the first-login window opens on the (virtual) CI machine and asks about the mouse. `make check` (Xvfb)
scrolls over the panel's volume icon: with natural scrolling a swipe up (scroll down) turns the volume up, with the
classic direction scroll up does.

## F6 / F7 brightness, F8 Project (external screen / projector), the Screenshot window, a new About (fix 11)

- **F6 / F7 = screen darker / brighter**, with the on-screen display, like F1–F3 for the sound (switch: *Settings →
  Keyboard & Shortcuts → Use F6, F7 and F8 as display keys*, key `fkeys_display`). The brightness keys of laptops do
  the same. **No brightnessctl or other tool is needed any more** (`src/hde-brightness.c`):
  1. the backlight of a laptop panel (`/sys/class/backlight`, firmware > platform > raw like GNOME), written directly
     when allowed, otherwise through **systemd-logind** (`Session.SetBrightness`, which any user may call for the screens
     of their own session), otherwise with brightnessctl / light / xbacklight if one is installed;
  2. on desktop monitors and in virtual machines, which have no backlight a program can change: **software dimming** of
     every screen through the XRandR gamma ramps (10–100 %), kept when the screens change.
  If neither is possible, a notification says why instead of an error. *Settings → Display* has a brightness slider.
  *Night Light* (it was a switch that did nothing) now makes the colours warmer (`night_light_temperature`, 4000 K).
- **F8 = Project**, also **Super+P** and the display key (Fn + the key with two screens), like Windows + P: a window with
  **PC screen only · Duplicate · Extend · Second screen only**. F8 again moves to the next choice (the open window is
  told, no second window), `Enter` or a click applies, `1`–`4` apply directly, `Esc` closes. Built on XRandR
  (`src/hde-randr.c`), no `xrandr` / `arandr` needed:
  - *PC screen* = the laptop panel (eDP / LVDS / DSI), or the first connected screen on a desktop PC or VM;
  - *Duplicate* uses the largest resolution every screen can show; *Extend* puts the other screens to the right at
    their native resolution (an arrangement made earlier is kept), one below the other if the graphics card cannot make
    the desktop that wide; *Second screen only* turns the computer's screen off — **a "Keep these display settings?"
    window goes back by itself after 15 seconds** unless *Keep changes* is clicked (the other screen may show nothing);
    pressing F8 again while it asks also goes back;
  - only one screen connected: the window says so (connect HDMI / DisplayPort / USB-C / VGA first).
  - **Plugging in a screen opens the window** (`display_connect=ask`; or `extend`, `duplicate`, `second`, `nothing` —
    *Settings → Display → When a screen is plugged in*). **Unplugging the screen in use turns the computer's screen back
    on**, and an unplugged screen never stays part of the desktop (`hde-xsettings`, HDE's display service). A projector
    forced on by hand while it reports "disconnected" (VGA without EDID, `xrandr --output VGA-1 --auto`) is left alone.
  - The choice is remembered for those screens and comes back at the next login.
  - The panel moves to the primary screen and the desktop covers the new size after every change; each screen gets the
    whole wallpaper.
  - CLI: `hde-settings --display-mode pc|duplicate|extend|second`, `--displays`, `--brightness [+N|-N|N]`;
    `hde-xsettings --status` shows the screens and the brightness method.
- **PrtSc** keeps taking screenshots with HDE's own program (no scrot): Print = whole screen, Shift+Print = area,
  Alt+Print = window, Ctrl + them = clipboard only. New: **Start menu → Screenshot** (`hde-screenshot --ui`) is a real
  program window — whole screen / active window / area, a delay, then the picture with *Copy*, *Save As*, *Open*,
  *Show in Folder*, *New Screenshot*.
- **About redone** (*Settings → About*, and *About HDE* in the desktop menu now opens it): HDE logo, version and build,
  the computer (name, model, processor, RAM, graphics, storage, screens), the system (OS, kernel, window manager, X
  server, GTK, uptime), **how much RAM HDE uses right now** (per program, PSS = shared libraries counted once, refreshed
  every 3 seconds) and what the whole system needs; *Copy system info*, *Project page*, *Session log*.
  `hde-settings --about` prints the same as text.

### Tests

`tests/display-test.sh` (CI job *screens*, Ubuntu 24.04): a whole session on Xorg with the dummy video driver, whose 16
RandR outputs act as connectors. F8 opens the window, F8 again moves on, Enter applies Extend (checked with `xrandr`:
the second screen to the right), the panel stays on the primary screen and the desktop covers both; F6/F7 dim both
screens (gamma: 95 %, 85 %, back to 100 %), `--brightness 60`, Night Light on and off; Duplicate; Second screen only
goes back by itself after the countdown, and stays when *Keep changes* is clicked (the panel moves to the other
screen); Super+P → PC screen only; the layout chosen earlier comes back when `hde-xsettings` starts; plugging in a
third screen opens the window; a fake backlight is used first (50 → 60 %, never fully off) and an unwritable one falls
back to software dimming. `tests/randr-plan-test.c` checks the layouts without an X server (laptop + projector,
desktop PC with two monitors, no common resolution, too few CRTCs, desktop size limit, unplugging the screen in use).
`make check` (Xvfb) checks F6/F7 and the OSD, the F8 window with one screen, the Screenshot window, and Settings →
About / Display.

## Start menu like Linux Mint or KDE, panel settings and extensions, About with the logo of the system, Wayland (fix 12)

- **Start menu redone** (`src/hde-startmenu.c`, *Settings → Start Menu*, key `menu_style`):
  - **Modern** (the default), like Linux Mint's Cinnamon menu: on the left your picture (AccountsService / `~/.face`,
    else your initial) and name, the places (Home, Desktop, Documents, Downloads, Music, Pictures, Videos), your
    favorite apps and the lock / log out / power buttons; on the right a search box, the categories — they open as the
    mouse moves over them — and the apps of the category with their descriptions; *Recent Files* as a category.
  - **Kickoff**, like KDE Plasma: picture, name and search on top, categories on the left with *Favorites* as a grid of
    tiles, the *Applications* / *Places* tabs (Tab switches) and *Sleep* / *Restart* / *Shut Down* / *Leave* below.
  - **Classic**: the previous drop-down menu.
  - Typing searches at once: names, descriptions, keywords and commands, accents optional, every word must match;
    a command in `$PATH` can be run. Up/Down/Left/Right/Tab/Enter, Esc clears the search then closes. Right-click or
    the Menu key on an app: *Add to / Remove from Favorites*, *Move Up / Down*, *Pin to Panel*, *Add to Desktop*.
  - On X11 it is a popup that holds the keyboard and mouse like a menu (a click outside closes it); it is built ahead
    of time so it opens at once.
- **Settings → Panel** (`src/hde-panel-config.h`), everything applies at once: **bottom or top**, height (24–64 px),
  opacity, each item on/off (Start button, Show Desktop, Run, pinned apps, taskbar — with or without window titles,
  grouping —, workspaces, tray, status icons, notifications, clock), 12/24-hour clock, date, seconds, a preview of
  the result. **Pinned apps** (also from the Start menu: *Pin to Panel*). **Extensions**: processor and
  memory use (built in, no tool) and the first line of any command, refreshed every few seconds, with presets
  (weather from wttr.in, uptime, free disk space, keyboard layout, public IP) and a command for clicks. The desktop
  icons, the OSD, notifications, the calendar and the search window follow the panel's edge and height.
  The Start button: label, and ☰ / the logo of the system / the HDE logo / no icon.
- **About with the logo of the system** (*Settings → About* and the new **About HDE** window of the desktop menu,
  `hde-settings --about-window`, which replaces the old GtkAboutDialog of image 4): the logo comes from
  `/etc/os-release` — first the system's own (`LOGO=`: Ubuntu `ubuntu-logo`, Hyggshi OS `distributor-logo`; icons
  like `distributor-logo-<ID>`, `emblem-<ID>`; `/usr/share/pixmaps/<ID>-logo.png`), else HDE's own copy
  (`data/logos/`, 27 distributions from Simple Icons, drawn by HDE's own SVG path reader `src/hde-svgpath.c`, no SVG
  library needed) or the Hyggshi OS logo drawn in code, else a round badge with the first letter in `ANSI_COLOR`.
  A derivative shows **"Based on Debian 13 (trixie)"** / **"Ubuntu 24.04 LTS (noble)"** with its base's logo
  (`ID_LIKE`, `HYGGSHI_BASE_CODENAME` / `UBUNTU_CODENAME`, `/etc/debian_version`), and the system's links (website,
  support, bug reports). `hde-settings --about` adds a *Based on* line.
- **The HDE (Wayland) session** (`data/hde-wayland.desktop`, needs `labwc`): `hde-start --wayland` → `hde-session
  --wayland` writes labwc's configuration from settings.ini (`hde-settings --wayland-config`: HDE's key bindings,
  touchpad, key repeat, keyboard layout, title bars in HDE's light/dark colours and accent) and starts labwc, which
  starts the session: the panel, desktop (wallpaper on every screen), Start menu, notifications, OSD, calendar and
  search as **layer-shell surfaces** (gtk-layer-shell), a **Wayland taskbar** (wlr-foreign-toplevel-management:
  click, middle-click to close, right-click Minimize / Maximize / Close; Show Desktop), the panel controlled over
  D-Bus, the keys as labwc key bindings that run `hde-hotkeys --action ...` (Super, Ctrl+Esc, F1–F3, F6–F8, media
  keys, PrtSc through `grim` / `slurp`, Super+L with HDE's own `hde-lock`, ...). Changing a setting rewrites labwc's
  configuration and labwc reloads it; logging out stops labwc. Without `libgtk-layer-shell-dev` HDE builds for X11 only.

### Tests

`make check`: the modern menu (places, search, Escape twice, Super / Super+S), *Add to Favorites* and *Pin to Panel*
from the app's menu, Enter starting an app, categories with the keyboard and under the mouse, Kickoff (Places tab) and
classic; the panel at the top 40 px high without the Run button, with two extensions (a command and the processor),
the Start button with the system's logo, the menu opening below the panel, a thin panel, back to the bottom; the About
window for Hyggshi OS (its logo, based on Debian 13), Debian, Linux Mint (based on Ubuntu 24.04) and an unknown system
(badge); `svgpath-test` draws every bundled logo; `hde-hotkeys --action` and `hde-settings --wayland-config`.
`tests/wayland-test.sh` (CI job *wayland*, Debian 13): the whole Wayland session in a headless labwc — see README.

## Control Center, battery panel, right-click menus with icons, the Start button's icon (fix 13)

- **Control Center** (`src/hde-control.c`): clicking the **network, Bluetooth or volume icon** or the notification
  bell, `Super+A` (`Super+N`: at the notifications) or `hde-panel --control-center[=PAGE]` opens one panel next to
  the status area:
  - **quick toggles** (two per row): Wi-Fi, Bluetooth, Airplane mode, Do Not Disturb, Dark mode, Night Light (X11,
    `hde-settings --night-light`), Power mode (power-profiles-daemon); a tile is coloured when it is on;
  - the arrow of **Wi-Fi** slides in the network list (NetworkManager): the network in use, saved ones, signal,
    security; a saved or open network connects at once, a **new secured one asks for its password in the list**
    (given to `nmcli --ask` on its standard input, never on the command line; a wrong password asks again and leaves
    no broken profile), Disconnect, Scan again, Network settings for hidden / enterprise networks;
  - the arrow of **Bluetooth**: the paired devices with Connect / Disconnect and their battery (BlueZ over D-Bus),
    the power switch, *Pair a new device…* (Settings);
  - **light and sound, each on its own row**: the screen brightness (`hde-settings --brightness`: the backlight
    through sysfs / logind, or software dimming), the volume with mute; its arrow opens the **Sound** page: the
    output devices (choosing one also moves the apps that play), the microphone (level, mute), the input devices and
    **the volume of each app** (pactl), *Advanced mixer…* (pavucontrol when installed);
  - the latest **notifications** with their icons (`src/hde-notify.c` now shares its history): click one to open it
    in its app, × removes it, *Clear all*; the bell's counter is reset;
  - the footer: the battery (opens the battery panel), screenshot, customize (*Settings → Panel*), settings, lock,
    power off.
  On X11 it is a popup that holds the pointer and keyboard like a menu (`src/hde-flyout.c`; a click outside or Esc
  closes it, Esc on a sub-page goes back), on Wayland a layer-shell surface with the keyboard. *Settings → Panel →
  Control Center* chooses the tiles and sections, and whether the icons open it (`cc_status_click=false`: network
  settings / mute as before).
- **Battery panel** (`src/hde-battery.c`, `src/hde-power.c`): clicking the battery icon shows the charge, whether it
  charges and the time left (to full, up to the firmware's charge limit), a chart of the charge (UPower's history, or
  what the panel saw since login), the draw in watts, energy, **health** (full charge now / when new), cycles,
  voltage, temperature, model and technology, each battery when there are two, the batteries of wireless devices
  (kernel and BlueZ), and the **power mode** buttons. The numbers come straight from `/sys/class/power_supply`
  (µWh or µAh, power or current × voltage): no upower needed. The battery icon's tooltip tells the time left too.
- **Right-click menus with icons** (`src/hde-flyout.c`: icons always shown, whatever `gtk-menu-images` says):
  - on the panel: Control Center, **Panel Settings…**, **Start Menu Settings…**, Position (bottom / top), Size,
    *Show on the panel* (every item, date, seconds), Add an extension…, Task Manager (when installed), Settings,
    About HDE, Power Off / Log Out…;
  - on the Start button: Open the Start menu, Menu layout (Modern / Kickoff / Classic), **Button icon** (the logo of
    the system, the HDE logo, ☰, an app grid, "start here" of the icon theme, none, other…), Button label, Start Menu
    Settings…, Panel Settings…;
  - on the network, Bluetooth, volume and battery icons: their page of the Control Center, their settings page, the
    tools that are installed (connection editor, Blueman, pavucontrol), mute / microphone.
- **The Start button shows the logo of the system by default** (`menu_button_icon=os`, it was the ☰ sign).
  *Settings → Start Menu → Start button* has an icon chooser (logo of the system, HDE logo, ☰, app grid, start-here,
  none, **Other…**: a picture of your own — PNG, SVG, JPEG — or any icon name, with a preview) and a preview of the
  button in the accent colour.
- **"I cannot find the Start menu / panel customization"**: it is in *Settings → Start Menu* and *Settings → Panel*,
  and now also one right-click away on the panel and on the Start button. If *Settings → About* shows an old build
  (the commit under *Build*), the programs that run are old copies: `make clean && make && sudo make install`, then log
  out and back in.

### Tests

`make check`: the Control Center opened from the volume icon (Wi-Fi, Bluetooth, Do Not Disturb, Dark mode and Night
Light tiles, the brightness slider dimming the screen, the volume slider, 2 notifications), the Do Not Disturb tile,
the Wi-Fi page with a password typed in the list (on nmcli's stdin, not on the command line), the Bluetooth page, the
Sound page (a second output device made the default, an app with its own volume), *Clear all*, Esc, `Super+A`; the
battery panel from the battery icon with a fake battery (82%, 5.2 W, 9 h 09 min left, health 91%, 123 cycles) and
the power mode switched to Power Saver (simulated power-profiles-daemon); the right-click menus of the panel, the
Start button (choosing the HDE logo changes the button) and the volume icon; `power-test` (µWh / µAh batteries,
charge limit, two batteries, a mouse, no battery). `tests/wayland-test.sh`: the Control Center and the battery panel
as layer-shell surfaces, Esc, `Super+A` through labwc.

## The panel sank below the bottom of the screen (fix 14)

**What happened:** once the panel had been made higher (*Settings → Panel → Height*, or *Size* in the panel's
right-click menu) and then lower again, it kept the bigger height but was still placed for the smaller one: its lower
part (window titles, the date under the clock, the bottom of the Start button) hung below the edge of the screen.

**Why:** libwnck's workspace switcher takes the height it was last given as its minimum height. After the panel had
been 56 px high, the switcher kept asking for 50 px, so the panel window could not get back to 34 px, while hde-panel
placed it at "screen height − 34 px" and reserved only 34 px for it. The test only checked what hde-panel *asked*
for, so it did not notice (in the CI screenshots the "28 px" panel was still 40 px high).

### Fixed

- **The panel is always exactly as high as set.** A container between the window and the items asks for exactly the
  panel's height and gives all of it to them (`HdeHeightBin` in `src/hde-panel.c`), so no item can make the panel
  higher. The workspace switcher now follows the panel instead of holding it up. The position, the space reserved for
  the panel (strut), the desktop icons and the pop-ups all use that height, so they now match the panel again.
- **The items fit any height:** the theme's minimum button height (24 px + padding) does not apply in the panel, thin
  panels (under 30 px) have no vertical padding, and the date goes under the time only when both lines fit with the
  fonts in use (measured). Otherwise time and date share one line.
- The number of notifications next to the bell is a round badge in any panel height (in a high panel it was stretched
  into a bar as high as the panel).
- **Wayland:** a panel made lower shrinks too (the layer surface is resized).
- With `HDE_DEBUG=1` the log tells where the panel window really is: `hde-panel: window: 0,1046 1920x34`.

The fix takes effect when hde-panel restarts: `hde-session restart` (desktop + panel, no logout), or log out and back
in.

### Tests

`make check` now checks the panel's window as the X server has it, not what hde-panel asked for (new
`tests/xtool.py geometry`): 34 px at the bottom at login, 40 px and then 28 px at the top, 56 px at the bottom and then
34 px again with its bottom edge on the bottom of the screen, 34 px reserved (`_NET_WM_STRUT_PARTIAL`), the clock on
one line in 28 px and the date back under the time in 34 px. Before the fix the "28 px" panel stayed 40 px high.

## Power, resolutions, DDC/CI brightness and the rest of the open items (fix 15)

### Added

- **Settings → Power** shows the battery (charge, charging or on battery, time left, draw, health, charge cycles, model,
  the firmware's charge limit; *Battery details…* opens the battery panel), the **power mode** (Power Saver / Balanced /
  Performance through power-profiles-daemon, also changed from the panel), the **battery saver**, the **low-battery
  warnings** and the screen timeout. The Battery and Battery saver sections only show on computers with a battery.
- **Battery saver** (`battery_saver`, off by default): on battery, at the charge chosen in *Turn on at* (10, 15, 20 %,
  30, 50 % or always on battery) the panel switches to the Power Saver mode, dims the screen to 70 % of its brightness
  (`battery_saver_dim`) and refreshes its extensions 3 times less often, with a notification. Plugging the computer in
  (or turning it off) puts the power mode and the brightness back, unless you changed them meanwhile. It stays on up to
  2 % above the level, so a charge going up and down around it does not switch it on and off.
- **Low-battery warnings** (`battery_warnings`, on by default): "Battery low" at 10 %, "Battery critically low" at 5 %
  (a critical notification that stays until closed), once per discharge; plugging in withdraws them.
- **Resolution, refresh rate and orientation** of each screen in *Settings → Display → Resolution*. The screens to the
  right of / below the changed one move along (no gap, no overlap); *Keep these display settings?* goes back by itself
  after 15 seconds. Kept choices go into `display_modes`, which the display service applies at every login, and the F8
  layouts use when they turn a screen on. Without a window: `hde-settings --display-set HDMI-1 1920x1080@60 [left]`,
  `... auto` for the native resolution.
- **DDC/CI brightness**: desktop monitors have no backlight in `/sys`, but most change their own brightness when asked
  over the video cable. With `ddcutil` installed, F6/F7, the slider in Settings and the Control Center use it (the
  monitors found are cached for the screens connected; `HDE_DDC=0` turns it off). Otherwise software dimming as before.
- The **software brightness** (no backlight, no DDC/CI) **is kept for the next login** (`~/.local/state/hde/state.ini`).
- **The mouse pointer in screenshots**: `hde-screenshot --pointer`, the *Show the mouse pointer* check box of the
  Screenshot window (remembered as `screenshot_pointer`, also for the Print key). On Wayland: `grim -c`.
- **GNOME's touchpad settings follow HDE's**: `org.gnome.desktop.peripherals.touchpad` / `.mouse` (and Cinnamon's) get
  the same natural scrolling, tap to click, speed and acceleration whenever HDE applies them (a change in Settings, every
  login), so Mutter and Muffin no longer undo HDE's choice. Only when those schemas and dconf are installed.

### Fixed

- **The Control Center's Wi-Fi, Bluetooth and Sound pages** were as tall as the main page, with empty space under
  their lists. Each page is now only as tall as it needs; the frame follows it.
- **`scripts/test-nexde`** was a pasted Markdown answer (code fences and all) that started programs which no longer
  exist (nexwm, nex-panel, …). It now starts a whole HDE session from `./build` in Xephyr, on its own D-Bus session bus,
  with a throw-away copy of your settings (`--fresh`, `--shared`, `--wm NAME`, `--check`).
- *Automatic suspend* in Settings → Power used to be a list that nothing applied. HDE does not suspend the computer by
  itself after a while without use (that needs a power manager, which also handles the cases where it must not, such as
  a film playing); the row now says so and opens the power manager's settings when one is installed.

### For Hyggshi OS

`scripts/build-nexwm.sh` in the Hyggshi-OS repository still builds the old NexDE (xcb, Qt 6, `bin/nexwm`) and writes a
session that starts `nex-panel` and friends, which this repository no longer has. `packaging/hyggshi-os/build-nexwm.sh`
is a drop-in replacement with the same variables (`NEXWM_REPO_URL`, `NEXWM_REF`, `SRC_DIR`, `PREFIX`, `DEBUG_MODE`): it
builds HDE, installs it into `/usr` with its two sessions, removes the old `nexwm.desktop`, keeps the libraries HDE needs
and removes the build tools. The CI runs it in a Debian 13 container and starts the installed session.

### Tests

`make check`: the battery saver on a draining fake battery (on at 18 %, the Power Saver mode of a simulated
power-profiles-daemon, 70 % brightness, the notification, off when plugged in with everything put back), the warnings at
9 % and 4 %, *Settings → Power*, `--power`, the software brightness written down and put back by a restarted display
service, the pointer in a screenshot (only there), GNOME's natural-scroll and tap-to-click following settings.ini;
`power-test` checks the decisions (levels, the 2 % margin, warnings once per discharge). `randr-plan-test`: new
resolutions with the screens beside / below moving along, Duplicate staying on top, rotation, `display_modes` read and
written, the wishes at login and in F8 layouts. `tests/display-test.sh` (real Xorg, dummy screens): `--display-set`,
the Display page's list (reverted when nobody answers, kept and remembered with *Keep changes*), the resolution coming
back at login, DDC/CI with a simulated `ddcutil`. CI also runs `scripts/test-nexde --check` and the Hyggshi OS script.

## Maximized windows under a top panel, the panel below the screen again, a black frame in Settings (fix 16)

**What was reported** (with two screenshots): with the panel at the top, a maximized window had no title bar and no
window buttons — they were under the panel — so it had to be made smaller again; after choosing *Bottom* the panel
sank below the edge of the screen again, with the wish that the panel measures the screen properly; and a black frame
in Hyggshi Settings, in Light mode too.

**Why:** the screenshots were taken with a build from before fix 14 (19:27 and 19:30 on 6 October; fix 14 is from
19:54). There the panel window was about 60 px high while it was placed for 34 px and only 34 px were reserved for it:
at the top it covered the title bars of maximized windows (the window manager kept 34 px free for it), at the bottom
its lower part hung below the screen. Fix 14 makes the panel exactly as high as set; this fix makes the panel check
where it really is by itself and put right whatever does not fit, whatever the cause.

The black frame came from GTK: a `GtkViewport` draws its content through an opaque buffer as soon as that content has a
background of its own, and the 14 px border around the sidebar's box (outside the box's background) was never painted
in it — black, in Light and Dark mode alike.

### Added

- **The panel measures the screen and itself** (`src/hde-measure.c`). After every placement (login, Settings, a screen
  plugged in, a new resolution, another window manager) it measures the screen straight from the X server — XRandR's
  monitors and the size of the X screen, so a resolution GTK has not caught up with yet does not matter — and its own
  window where the X server really has it, with the space it reserves (`_NET_WM_STRUT_PARTIAL`). Put elsewhere by the
  window manager: it moves back. Not as high as set: it asks again, and if the window keeps another height it is placed
  for that height (all of it on the screen) and that height is reserved, so maximized windows still keep clear of it.
  Reserved space too small: set again. A panel moved or resized later on (by a window manager, a script) is noticed and
  put back too. All in device pixels, so `GDK_SCALE=2` reserves the right space. The log tells what was measured:
  `hde-panel: measured: screen 1920x1080 (eDP-1) at 0,0 (from GTK, X screen 1920x1080); panel 0,1046 1920x34, 34 px
  reserved at the bottom: fits`.
- **Settings → Panel → Position and size → Screen**: the screen (size, connector, text scale), where the panel really
  is and how high, the space kept free for it and the room windows get; a warning in red when something does not fit,
  and **Measure again**, which has the panel measure the screen and put itself right.
- **`hde-panel --measure`** prints the same (screen, panel window, reserved space, `_NET_WORKAREA`) once the panel has
  measured again; exit status 0 when it fits, 1 when not, 2 without a panel.

### Fixed

- **No black frame around the sidebar of Settings** any more: the sidebar's background is on the scrolled window
  around it, and the box of buttons inside has none.

To get it: `git pull`, `make && sudo make install`, then log out and back in (or `hde-session restart`, then reopen
Settings).

### Tests

`make check`, under Metacity and under Openbox (after the live switch: the setup of the screenshots): a maximized window
with the panel at the top (40 and 28 px; 34 px under Openbox) begins below the panel, with the panel at the bottom (56
px; 34 px under Openbox) it ends above it (new `tests/xtool.py maximize` and `frame`: the frame from
`_NET_FRAME_EXTENTS`); the panel moved away (`xdotool windowmove`) or made higher (`xdotool windowsize`) puts itself
back; the measurement at login, `hde-panel --measure`, *Settings → Panel → Screen* and *Measure again*; the sidebar of
Settings without black pixels in Light and Dark mode (`tests/xtool.py pixel`). `measure-test` checks the measuring
without an X server: where the panel belongs, the reserved space on one or two screens and with scale 2, and what is
wrong with a panel too high, too low, off to the side, or with too little or no space reserved.

## The frame and the colours of Settings, and a file manager: Hyggshi Files (fix 17)

The Settings window of the report still had the black frame around its sidebar: it came from a build older than fix 16
(Settings → Panel had no *Screen* row yet), so `git pull`, `make && sudo make install` and logging out and back in
fixes it. The colours that did not match were HDE's blue accent next to the orange switches and selections of the GTK
theme (Yaru): HDE drew its own highlights in its accent colour, the theme drew its widgets in its own.

### Changed

- **The accent colour is "Automatic" by default**: the selection colour of the GTK theme in use
  (`theme_selected_bg_color`: Yaru orange, Adwaita blue, Adwaita-dark darker blue), so the sidebar of Settings, the
  Start menu, the desktop's selection frame and the panel's highlights match what the theme draws — and it follows
  when the theme changes. *Settings → Appearance → Accent color* is a list with swatches: *Automatic (from the theme)*,
  then the colours; a chosen colour now also goes on the switches, sliders, progress bars, selected rows, suggested
  buttons and text selection of the GTK theme, so both always agree. `accent=auto` (or no `accent`) in `settings.ini`.

### Added

- **Hyggshi Files** (`hde-files`), the file manager, in its own folder `hde-files/` (sources in `hde-files/src/`, its own
  `Makefile`; the top-level `make` / `make install` build and install it too). Icons with thumbnails or a sortable list,
  tabs, the places sidebar, a path bar or a typed location, search in subfolders, hidden files, zoom; copy / move with
  progress, Cancel and Replace / Skip / Keep Both / Merge; drag and drop; the Trash with Restore (the freedesktop.org
  trash, no GVfs needed); Undo; rename, new folder / document, Properties with permissions, Open With, compress /
  extract, a terminal there; right-click menus with icons like the rest of HDE; Light / Dark and the accent of HDE.
  It is HDE's file manager: `hde-mimeapps.list` makes folders open in it in HDE sessions (the desktop, the Start menu's
  places), `Super+E` starts it first, the Screenshot tool's *Show in Folder* uses `hde-files --select`, and while it runs
  it answers `org.freedesktop.FileManager1` for browsers and other programs.

To get it: `git pull`, `make && sudo make install`, then log out and back in (or `make reload`).

### Tests

`make check`: with Yaru and Dark mode the Automatic accent is Yaru's orange (`#e95420`, logged by Settings) and the
sidebar has no black pixels, then it follows back to Adwaita. Hyggshi Files: a folder with a hidden file (5 items
shown), the thumbnail of a picture on the screen and in `~/.cache/thumbnails`, `Ctrl+H`, `Ctrl+2` / `Ctrl+1`, a new
folder, type-ahead + *Rename…* from the Menu key (notes.txt -> readme.txt; `F2` is HDE's volume-down key there), `Ctrl+C` / `Ctrl+V` ("readme (copy).txt") and `Ctrl+Z`, `Delete`
to the trash with its `.trashinfo`, the Trash view and Restore, `Alt+Left`, the right-click menus of a file and of the
folder, Properties of a folder with its size, `Ctrl+F` finding files in subfolders, a double click into a folder and
`Backspace`, tabs, `ShowItems` over D-Bus, `hde-files --select`, Dark mode, `--quit`, no GTK criticals, and a double
click on a folder of the desktop opening Hyggshi Files.

## HDE's own lock screen, on both sessions (fix 18)

`Super+L` used to hand the session over to somebody else's locker (swaylock, gtklock, i3lock, slock, …) and, on a
machine without one, to `loginctl lock-session`. HDE now locks its sessions itself: `hde-lock` (sources
`src/hde-lock.c`, `src/hde-lock-core.c` for the clock, the date, the name and the password field, and
`protocols/ext-session-lock-v1.xml` for the Wayland side) is the first thing `HDE_SH_LOCK` runs on both of HDE's
sessions. The other lockers stay where hde-lock cannot lock at all — a build without PAM, a compositor without the
session lock protocol — and `hde-lock --check` says which of the two this session is (exit 0 = hde-lock can lock it).

### Added

- **The X11 session: a window of its own.** The lock screen is an override-redirect window over the whole screen that
  no window manager can move, decorate, draw over or take the keyboard from, with the keyboard and the mouse grabbed
  and the pointer invisible; `TERM`, `INT`, `HUP`, `QUIT` and `USR1` do not unlock it, and `_HDE_LOCKED` on the root
  window tells the rest of HDE (and the tests) that the screen is locked. The keyboard is what the lock screen insists
  on — without it the keys would go to whatever window has the focus — while a program that holds the *pointer* for a
  moment (a menu, a flyout, the window manager in the middle of a drag) only gets the mouse retried while the screen
  is locked, instead of the lock failing. A click is received by the lock screen itself (`INFO: a click on the lock
  screen: it went to the lock screen, not to the session behind it`), and the password is checked through PAM.
- **The Wayland session: the compositor's session lock.** `ext-session-lock-v1` (labwc offers it): the compositor
  itself stops showing and feeding input to every other program, one `wl_shm` buffer per output, the clock, the date
  and the name drawn on it. A lock client that is killed from outside does *not* give the session back — only the
  password does; that is what the protocol is for.
- **`hde-lock` also takes the way out**: `--check` (0 = this session can be locked, 3 = it cannot, with the reason in
  one line), `--version` (which backends this build got), `--x11` / `--wayland` to pick the session by hand — without
  them it is `WAYLAND_DISPLAY` first, then `DISPLAY`. The password means `/etc/pam.d/hde-lock` when that file exists
  and `login` otherwise, so a system can say there what locking means, and the file that ships the program does not
  have to.

### Fixed

- **A cursor needs a real pixmap, not `None`.** The invisible pointer was made with
  `xcb_create_cursor(conn, cursor, XCB_NONE, XCB_NONE, …)`. The X protocol's `CreateCursor` is
  `source: PIXMAP, mask: PIXMAP or None`: only the *mask* may be `None`, so the X server refused the request
  (`BadPixmap`), the cursor never existed, and everything built on it went with it — the lock window was refused
  (`BadCursor`) and the pointer grab as well, which made the lock screen look like "another program is holding the
  keyboard" and left the screen unlocked. The cursor is now made from a 1×1 depth-1 pixmap with every bit clear (a
  cursor the screen shines through), and a refusal says what it was: the reason of a grab that did not work and the
  name of the X error (`BadAccess`, `BadCursor`, …) are in the log.

### Tests

`make check` (the screens job) locks the session through `HDE_SH_LOCK` — the same thing `Super+L` and the Power menu's
Lock button do — and checks the lock screen's own background on the screen, that the click goes to the lock screen,
that `TERM` does not unlock it, that a password PAM turns down is refused and only `Enter` is checked, and that a
test PAM module accepts only the typed test password: 16 assertions, including `hde-lock --check`. The assertions
of the locked state only run while the screen is really locked (an unlocked desktop used to pass some of them by
itself). The Wayland job does the same on the compositor's session lock (`ext-session-lock-v1`) and checks that the
compositor shows nothing but the lock screen, that `pam_deny` keeps it locked and that the correct test password
unlocks it. Fedora checks that `pam-devel`,
`libxcb-devel`, `wayland-devel` and `libxkbcommon-devel` really built all three backends of `hde-lock` there.

## The login screen: the field to type the user name in (fix 19)

The HDE login screen (`login/sddm/hde`, an SDDM theme) could come up with no user tiles *and* no field to type a user
name in: the card showed the password field, the *Sign in* button answered *Please choose a user.* and there was
nothing to type into at all. The screen was unusable, and its Enter key complained instead of giving the keyboard to a
field.

### Fixed

- **A user list that is empty is not a user list.** SDDM builds the greeter's `userModel` from `getpwent()`, leaving
  accounts out by uid range (`[Users] MinimumUid` / `MaximumUid` in `/etc/sddm.conf.d`) and by `HideUsers` /
  `HideShells`; on a machine where that comes out empty, `HdeUsers` was 0 pixels high while `theme.conf` said
  `userMode=user`, so no tiles and no name field were drawn and `userName` stayed empty: the login screen could not
  name anybody. `Main.qml` now decides from what the greeter actually gave (`haveUserTiles` = tiles are shown *and*
  `users.count > 0`) and falls back to a **user name field** (`showNameField`) whenever there is no tile list to show,
  or when `userMode=username` is asked for. The card says why the list is missing ("This login screen has no user
  list: type your user name."), `tryLogin()` reads the typed name, and `sddm.login()` is called with it — typing a name
  and pressing Enter now logs in.
- **The keyboard goes into the card.** With the greeter having taken the keyboard for its own window, `focus = true`
  was not enough — the fields stayed unfocused, so typing went nowhere and the card's own Enter handler ran with the
  keyboard outside it (which is where *Please choose a user.* came from). `HdeField`/`HdePassword.focusField()` use
  `forceActiveFocus()` now, `Main.qml` puts the keyboard in the first field when the theme is loaded, when the card is
  clicked and from a 600 ms watchdog `Timer`, and Enter with the keyboard nowhere focuses a field instead of
  complaining. The names of the fields are in the greeter's log (`hde-login: the greeter listed 1 user(s)`,
  `hde-login: the greeter listed no users: the login screen offers a user name field`, `hde-login: the keyboard is in
  the password field`) so this is diagnosable from `journalctl -u sddm`.
- **A `ReferenceError` in every greeter without a keyboard object.** `HdeLayouts.qml` connected to `keyboard` without a
  guard; SDDM only has that object from 0.19 on, and on an older one the connection was a QML error. And
  `closeChoosers()` called `layouts.closeCombo()` while `HdeLayouts` had no `id` — no session or layout popup could be
  closed. Both fixed; `theme.conf` documents `showUserList` separately from the user mode.

### Tests

`tests/sddm-test.sh` now checks what the card decided from what the greeter gave it (the log lines above), where the
keyboard is, that typing reaches the card (xdotool, when the WM-less CI X server delivers it) and renders the screen a
second time with `[Users] MinimumUid=60000` written into `/etc/sddm.conf.d` so the greeter really does hand over no
users: the name field has to be there, and the card still drawn. `tests/sddm-qml-test.py` (new, PySide6/Qt 6,
offscreen, no display, no SDDM, no root) loads `Main.qml` with a greeter made in the test and checks three
situations end to end — one user (tiles, keyboard in the password field), an empty user list and no user model at all
(the name field appears, the keyboard goes into it, typing a name reaches `sddm.login()`): 27 checks, run by
`sh tests/sddm-test.sh` and installed in the SDDM job of the CI. `pyside6-qmllint` on every QML file: no errors.

The greeter runs on a pty in the test (`tests/ptylog.py`): SDDM's greeter only writes the theme's own output to stderr
when it is on a terminal and sends it to journald otherwise (Fedora's SDDM, in a CI container without a journal: the
log the test reads stays empty), which is what the Fedora job caught.

## Fedora and Arch Linux, and the pictures of the login screen (fix 20)

Three things a user asked for in one go: the login screen of HDE *seen*, not only described; the Fedora job doing what
the Ubuntu job does (a whole desktop test, with pictures); and Arch Linux supported as a distribution, with the way it
is done written down.

### Added

- **Pictures of the login screen.** `tests/sddm-qml-test.py` now photographs the theme as it renders, in every case it
  checks: with the users as tiles, with an empty user list (the field to type a user name in, which is what fixes 19 is
  about) and with no user model at all. They are `shot-sddm-*.png` in `$HDE_TEST_OUT`, they travel in the
  `hde-sddm-login` artifact of the CI job, and the job publishes them as check runs (`hde-shot sddm-1`, …) when the
  commit message says `[shots]` — so the login screen can be looked at without logging out of one's own session, and
  without downloading anything.
- **Fedora runs the smoke test, and gets the same pictures.** The Fedora job (`Fedora (dnf, full desktop)`) now runs
  `tests/smoke.sh` — the same test the Ubuntu jobs run — after its own Fedora test: the whole desktop in Xvfb (the
  panel, the Start menu, the Control Center, Settings, notifications, the desktop icons) *photographed* along the way,
  the results published as annotations and the pictures in the **`hde-smoke-fedora`** artifact — the Fedora twin of
  `hde-smoke-ubuntu-22.04`. Its check runs are labelled
  (`hde-shot Fedora 01-panel`) so Fedora's, Ubuntu's and Arch's pictures of the same test are told apart.
- **Arch Linux: `packaging/arch/README.md`, `packaging/arch/deps.sh` and a CI job.** Arch names its development
  packages like the libraries (gtk3, libwnck3, libxcb, pam, wlroots: no `-dev`, no `-devel`), its Python package is
  `python`, xrandr is a package of its own (`xorg-xrandr`) and its windows are in `xorg-xset`/`xorg-xsetroot` — all of
  that is in the list, and `hde-settings --deps` prints `sudo pacman -S` with those names on an Arch (the translation
  table in `src/hde-distro.h` got the rows it was missing: `python3` → `python`, `xsltproc` → `libxslt`, and `xrandr`
  in the `x11-xserver-utils` row; `tests/distro-test.c` checks the three of them without an Arch machine).
  `packaging/arch/README.md` has the package table (Debian ↔ Arch) and a word on the **rolling wlroots**: Arch carries
  the newest release, `nexwm/src/wayland.c` supports 0.17 and newer (`-DNEXWM_WLROOTS_MINOR`), and the new CI job
  builds the compositor against whatever Arch has today — a wlroots release that breaks the source is found there
  first. `tests/arch-test.sh` checks the list everywhere (off an Arch it checks the list in the repository and skips
  the machine-specific half), and on an Arch it checks pacman, the translation, and that NexWM was built with its
  wlroots compositor. `make check-unit` runs it, `make check-arch` is the same test on an Arch.
- **An Arch CI job** (`Arch Linux (pacman, current wlroots)`, `container: archlinux:latest`): install through
  `packaging/arch/deps.sh install build runtime-minimal test`, build every target (and say which wlroots that was),
  `tests/arch-test.sh --deps`, the smoke test in Xvfb (the pictures of the Arch desktop: the **`hde-smoke-arch`**
  artifact), the unit tests, and the pictures as check runs under `[shots]` (named `hde-shot Arch …`, and Fedora's
  `hde-shot Fedora …`, so the three systems' pictures of the same test are told apart).

### Tests

`tests/sddm-qml-test.py` did not need a display before and does not need one now: the pictures come from the QML
engine itself (`QQuickView::grabWindow()` with the offscreen platform and the software backend — the same rendering
the CI greeter uses). Locally: 30 checks, including "a picture of the login screen was saved" three times.
`tests/arch-test.sh --deps` passes anywhere (11 checks); on an Arch it is 20 more (pacman, the package names,
`nexwm --version`, the unit tests). `tests/distro-test.c`: 53 checks, three of them new for the Arch names.

## The first Fedora and Arch runs: what failed, and the pictures (fix 21)

The two commits before this one (`15d7c86`, `0a8df83`) took the new jobs to the CI and both jobs failed. The failures
were read from the check-run annotations (the job logs themselves are not reachable through the API): the whole results
body of a smoke test arrives as one annotation, so `tests/ci-annotate.py`'s output is what a run is diagnosed from.

### Arch: `Install the dependencies (packaging/arch/deps.sh)` — exit 127 and exit 1

The container of `archlinux:latest` has just been unpacked and its `/var/lib/pacman/sync` is empty: `pacman -S` cannot
find a single name, and the fallback of `deps.sh` (`pick` takes the first alternative and says that pacman has no
package information) installed nothing. The job then ran the report step without `python3` — that is the exit 127
(`command not found`), and the upload step warned that it found no `/tmp/hde-smoke/*` and no `build.log`, which is why
no `hde-smoke-arch` artifact exists.

* `packaging/arch/deps.sh`: `install` runs `pacman -Sy --noconfirm` first when there is no sync database, with a line
  saying why (nothing changes on an installed system, the databases are there).
* The Arch job's report step checks for `python3` and says that a step before it did not get to install the packages
  instead of failing with `command not found`.

### Fedora: the smoke test's failures

The same smoke test the Ubuntu jobs run passes there; on Fedora these checks failed, and this is what each one was:

* **F6/F7/F8** ("F6 shows the brightness OSD", "F6 dims the screen to 95%", "the panel got the level for its OSD",
  "F7 brightens it again", "fkeys_display=false gives F6/F7/F8 back to applications", "F8 opens the Project window"):
  the brightness code itself works on Fedora (the Settings slider's software dimming checks all pass), so the key path
  is what differs. The smoke test now writes the reason `hde-hotkeys` itself logs into the results whenever one of these
  checks fails (`keys_reason`): whether another program holds the keys and which, and what the brightness action said.
* **"... the date under the time"**: the panel puts the date under the time only when both lines fit (it never grows, an
  item higher than the panel would be cut), and the line heights are font metrics. Fedora's font needs 34 px for the
  two lines where the panel has 28, so the panel kept one line and said the numbers — the check now reads those numbers
  and passes when the panel has really measured this. `src/hde-panel.c` also logs *which* font decided them.
* **"Settings pairs a Bluetooth device"**: the Pair button of the "Other devices" card was clicked at one exact place
  (1078,556), which the rows of that card move when the font of the system is another one. The test now clicks a small
  ladder of positions around it and stops as soon as the mock reports the device paired.
* **"crashed panel is restarted by hde-session"** and **"logout stops the panel / hde-hotkeys"**: these looked once
  after a fixed 4 s / 5 s. They now wait for the event (up to 15 s) and, when it does not happen, print what is still
  running into the results — a slower machine fails with a diagnosis instead of a bare FAIL.
* **"right-click on a file: ... Compress ..."**: the menu of the file manager is read once after the click; on a loaded
  machine the first read can catch half of it. It is read again (up to 3 s) before the check decides.

### The font the CI measures with (`HDE_SMOKE_FONT`)

The checks of `tests/smoke.sh` are in pixels (the clock of the panel on one line, the 1020x700 Settings window, the
place of the Pair button) and the desktop these numbers were written for uses the font of an Ubuntu runner. A system
whose default font is another one measures another desktop. `tests/smoke.sh` now reads `HDE_SMOKE_FONT` ("DejaVu Sans
10") and writes it into the settings the session starts with; the Fedora and Arch jobs set it, so all three systems
measure the same desktop and their pictures are of the same thing. It is unset by default: on a developer's machine
nothing about the test changes.

### Where the pictures are

* the artifacts of a run: **`hde-smoke-ubuntu-22.04`**, **`hde-smoke-fedora`**, **`hde-smoke-arch`** (the `shot-*.png` of
  `tests/smoke.sh` plus its logs) — they are uploaded even when the job fails, which is how the Fedora pictures of the
  failing run could be looked at;
* and, when the commit message contains `[shots]`, one check run per picture (`hde-shot Fedora 01-desktop`, ...), so
  the pictures of a run can be looked at in the browser without downloading the artifact.

## Fresh Arch containers: fully update before installing packages (fix 22)

The Arch job for `2fc16f1` failed during `Install the dependencies (packaging/arch/deps.sh)`. The job log was not
available through the Actions API while the workflow was still running, so the exact pacman error could not be read.
The fresh-container path now runs `pacman -Syu --noconfirm` instead of syncing repository databases alone, avoiding a
partial upgrade before installing dependencies. If pacman has sync databases but none of an entry's package alternatives
exist, the installer now reports that explicitly rather than passing a stale package name onward. A mock-pacman test in
`tests/arch-test.sh --deps` exercises the empty-database bootstrap without needing an Arch host.

On that same run, Fedora's build and Fedora-specific session test succeeded; its full smoke test was still running at the
last status check. No Fedora-only source or dependency change was indicated by those results.

## Idle to sleep, USB drives and the keyring (fix 23)

The three things a desktop is expected to do on its own were left to something else: the screen when nobody is at the
computer went to `xfce4-power-manager`, a USB stick was mounted by whatever GVfs the file manager happened to have, and
there was no keyring at all — so the Wi-Fi password, the browser's and Git's were asked for again at every login. This
is HDE's own answer to all three, in the same style as the rest of HDE: one small program each, no new dependency that
the distribution does not already ship, and usable on the Wayland session as well as on the X11 one.

### Added

* **`hde-idle`** — `src/hde-idle.c`, with the rules in `src/hde-idle-core.h/.c`. Four times, set in
  *Settings → Power → Screen and sleep*: when the screen turns off, when the session locks, and when the computer
  sleeps — the last one separately for battery and while plugged in (1 minute to 2 hours, or never). The order is
  always screen off, then locked, then asleep, and the computer locks before it sleeps even while a video is holding
  the screen on. A player that asks to stay awake (`org.freedesktop.ScreenSaver.Inhibit`, which is what Firefox, mpv
  and VLC send) holds off the screen and the lock but **not** the sleep; a program that asks not to be suspended
  (`org.freedesktop.PowerManagement.Inhibit`, a presentation, a long download) holds off the sleep only. Both are
  dropped when the program that asked disappears, so a crashed player cannot keep the screen on forever.
  On X11 the time away comes from the X Screen Saver extension and the screen is turned off with DPMS; on Wayland it
  asks the compositor (`ext-idle-notify-v1`) — `WAYLAND_DISPLAY` is tried first and X11 is the fallback.
  `hde-idle --check` is what `hde-settings` asks: when the time away cannot be measured here (no extension, a
  compositor without the protocol), Settings hands the screen back to the power manager that is already installed and
  says so on the page, so a setting that nothing follows can no longer leave the screen on.
* **`hde-automount`** and the **Drives** list — `src/hde-udisks.h/.c` (the talk with **udisks2** over D-Bus, written
  here: no GVfs volume monitor, no libudisks2), `src/hde-disks.h/.c` (which drives are shown, which of them are
  mounted, which can be taken out), `src/hde-automount.c` (mounting when a drive is plugged in) and
  `hde-files/src/disks.c` (the list in the sidebar of Hyggshi Files). Plugging in a stick mounts it and says so with a
  notification whose *Open* runs Files on it; in Files it is in the sidebar, and the button beside it safely removes
  it ("It is now safe to remove the drive."). Loop devices, the system disk, encrypted volumes that need a password
  and an optical drive with no disc in it are not shown; the same list works from a script (`hde-automount --list`).
* **`hde-keyring`** — `src/hde-keyring.c`, with the environment parsing in `src/hde-keyring-core.h/.c`.
  `gnome-keyring-daemon` is started by `hde-session` **before** anything that could ask for a password, on both the
  X11 and the Wayland session, and the socket it answers on (`GNOME_KEYRING_CONTROL`, `SSH_AUTH_SOCK`) is exported
  into the D-Bus activation environment. With the two lines of `packaging/pam/hde-keyring` in the PAM file of the
  display manager, the password typed at the login screen opens the keyring as well and nothing asks a second time;
  without them the keyring is there but locked, which is why the note is printed by `make install` and explained in
  the README.
* **Unit tests** for all three — `tests/idle-test.c`, `tests/disks-test.c`, `tests/keyring-test.c`, in `UNIT_TESTS`
  and run by `make check-unit`. This is the shape the other new pieces of HDE follow from now on: the decisions live
  in a `src/hde-*-core.c` that the program and the test both use, so they can be checked without a display, a drive
  or a keyring.

### Fixed

* **The committed object file.** `hde-core/notifications/notifications.o` was in the repository (a build result, not
  source). It is `git rm`'d, `hde-core/**/*.o|*.a|*.so` is in `.gitignore`, and `make check-tree` — now part of
  `make check` — fails when any built file has been committed again.
* ***Settings → Power*** mixed the three times into one `screen_timeout` with only 0/5/10/15/30/60 to choose from, and
  asked for `xfce4-power-manager` when it was not installed. It now has the four combos above (1 minute to 2 hours),
  the *Lock before sleeping* switch, and only mentions a power manager when this machine actually has one. An old
  `screen_timeout` is read as the new times the first time, so nobody's setting is lost.
* **Build and packaging:** the Wayland session needs `wayland-scanner` and `libwayland-dev` (the CI job did not have
  them), and `udisks2` / `gnome-keyring` / `libxext` are in the dependency lists (`packaging/arch/deps.sh`,
  `packaging/fedora/deps.sh`, `hde-settings --deps`).

### Tests

`make check-unit` runs the 148 checks of the three new tests (67 idle, 48 disks, 33 keyring); `make check-tree`
verifies that no build result is tracked. The programs themselves are compiled but not linked in this sandbox (there
is no GLib, GIO, X11-ext or Wayland header here), so the first real compile and link is the CI job — that is what the
dependency additions above are for.
