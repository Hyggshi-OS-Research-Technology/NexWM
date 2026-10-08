# HDE on Fedora

HDE (the Hyggshi Desktop Environment, this repository) builds and runs on Fedora — Fedora 41, 42 and Rawhide are
tested in CI, and Fedora-based distributions (Nobara, Ultramarine, Bazzite, RHEL, AlmaLinux, Rocky) are recognized as
the RPM family too. Nothing here is Debian-only any more: HDE tells the user the command of *their* system
(`sudo dnf install …`, not `sudo apt install …`) and the package names of their system
(`gtk3-devel`, not `libgtk-3-dev`).

## Build and install

```sh
# what compiles HDE, plus what the session needs to start
sh packaging/fedora/deps.sh install build runtime-minimal
# the programs HDE's features use when they are there (Wi-Fi, sound, power, the Wayland session, screenshots)
sh packaging/fedora/deps.sh install runtime-full

make -j"$(nproc)"
sudo make install            # PREFIX=/usr/local by default: use PREFIX=/usr to be a distribution package
```

Then log out and choose **HDE** (X11) or **HDE (Wayland)** on the login screen. Logs: `~/.cache/hde/session.log`.
`packaging/fedora/deps.sh` with no arguments prints its groups; `list GROUP` prints the package names of one group
(useful to write a `.spec`), `missing GROUP` prints what is not installed.

`hde-settings --deps [build|runtime|test]` prints the same lists *from inside HDE*, for the system it runs on: on
Fedora the commands say `dnf`, on Debian/Ubuntu/Hyggshi OS `apt`, on openSUSE `zypper`, on Arch `pacman`.

## Fedora package names

The names in the README of this repository are Debian's. On Fedora they are:

| Debian / Ubuntu              | Fedora                                  |
|------------------------------|-----------------------------------------|
| `build-essential`            | `gcc gcc-c++ make`                      |
| `pkg-config`                 | `pkgconf-pkg-config`                    |
| `libgtk-3-dev`               | `gtk3-devel`                            |
| `libwnck-3-dev`              | `libwnck3-devel`                        |
| `libgtk-layer-shell-dev`     | `gtk-layer-shell-devel` + `wayland-devel` |
| `wayland-protocols`          | `wayland-protocols-devel`                |
| `libxi-dev`                  | `libXi-devel`                           |
| `libxrandr-dev`              | `libXrandr-devel`                       |
| `libxfixes-dev`              | `libXfixes-devel`                       |
| `libxcomposite-dev`          | `libXcomposite-devel`                   |
| `libxkbcommon-dev`           | `libxkbcommon-devel`                    |
| `libinput-dev`               | `libinput-devel`                        |
| `libpixman-1-dev`            | `pixman-devel`                          |
| `libcairo2-dev`              | `cairo-devel`                           |
| `libwlroots-dev`             | `wlroots-devel`, `wlroots0.19-devel`, `wlroots0.18-devel`… (the name follows the release) |
| `libvte-2.91-dev` (optional) | `vte291-devel`                          |
| `metacity` / `openbox`       | `metacity` / `openbox`                  |
| `network-manager`            | `NetworkManager`                        |
| `policykit-1-gnome`          | `polkit-gnome`                          |
| `libnotify-bin`              | `libnotify`                             |
| `xvfb`, `x11-utils`          | `xorg-x11-server-Xvfb`, `xprop xwininfo` |
| `xserver-xephyr`             | `xorg-x11-server-Xephyr`                |
| `xwayland`                   | `xorg-x11-server-Xwayland`              |

`packaging/fedora/deps.sh` also knows the packages Fedora split up over the releases (the X tools that used to be
`xorg-x11-server-utils`) and picks the first name this Fedora has.

## What is Fedora-specific in HDE

* **Every message.** `src/hde-distro.h` reads `/etc/os-release` and turns a missing program into the right command:
  `hde-session` says `sudo dnf install metacity` on Fedora and `sudo apt install metacity` on Debian, `hde-settings`
  says `sudo dnf install NetworkManager` (Fedora's name) instead of `network-manager` (Debian's), and so on for the
  screenshot tool (`grim`, `slurp`), the media keys (`playerctl`), the brightness (`ddcutil`), the power mode
  (`power-profiles-daemon`), the polkit agent (`polkit-gnome`) and the XInput 2 packages (`libXi-devel`).
* **Settings → About** names the system the user is on, with the logo Fedora installs (`fedora-logo-icon`), and the
  "How much RAM is needed?" text is Fedora's advice on Fedora (2 GB, 4 GB recommended) instead of Debian's.
* **The SDDM login screen**: `login/sddm/hde/` is a theme of its own (Qt 5 and Qt 6 — Fedora ships a Qt 6 SDDM);
  `sh login/sddm/install.sh` installs it and can make HDE the session SDDM starts.
* **The Wayland session** uses labwc (`dnf install labwc`), like on Debian; NexWM's own compositor (`nexwm/`) is built
  when `wlroots-devel` is there and appears as a session of its own.
* **SELinux**: `make install` copies files into `/usr/local/...` (or `/usr` with `PREFIX=/usr`); if a policy complains,
  `sudo restorecon -Rv /usr/local/bin /usr/local/share/hde` fixes the labels (files installed by a `.rpm` get them
  from the package policy).

## Tests

```sh
make check-unit              # the unit tests: no X server, no desktop packages (also runs on a base Fedora)
sh tests/fedora-test.sh --full   # a Fedora with the desktop packages: a whole session in Xvfb, the Fedora names,
                                 # the SDDM theme, the Wayland session with labwc
sh tests/fedora-test.sh --base   # a *base* Fedora (only the build dependencies): HDE builds, the unit tests pass,
                                 # the session starts without a window manager and says so, with dnf hints
sh tests/smoke.sh            # the full feature test (Debian's CI job; portable, but the Debian packages are assumed)
```

CI runs both of them separately: the **Fedora (dnf, full desktop)** job and the **Fedora base (minimal, no desktop
packages)** job in `.github/workflows/build.yml`, next to the Ubuntu, Debian and Wayland jobs.
