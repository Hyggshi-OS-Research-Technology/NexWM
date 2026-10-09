# HDE on Arch Linux

HDE (the Hyggshi Desktop Environment, this repository) builds and runs on Arch Linux — Arch itself and the Arch-based
systems (Manjaro, EndeavourOS, Garuda, CachyOS, Artix: `src/hde-distro.h` recognizes all of them as the Arch family),
tested in CI in a fresh `archlinux` container on every push.

## Build and install

```sh
# the same list the CI uses, through pacman (-S --needed --noconfirm)
sh packaging/arch/deps.sh install build runtime-minimal
# what HDE's features use when they are there (Wi-Fi, sound, power, the Wayland session, screenshots, SDDM)
sh packaging/arch/deps.sh install runtime-full

make -j"$(nproc)"
sudo make install            # PREFIX=/usr/local by default: use PREFIX=/usr to be a distribution package
```

In a fresh container (no sync database yet), `deps.sh install` runs `pacman -Syu` first, syncing the repositories and updating
base packages before it installs HDE's dependencies. Installed systems with sync databases are left alone.

Then log out and choose **HDE** (X11) or **HDE (Wayland)** on the login screen. Logs: `~/.cache/hde/session.log`.
`packaging/arch/deps.sh` with no arguments prints its groups; `list GROUP` prints the package names of one group
(useful to write a `PKGBUILD`), `missing GROUP` prints what is not installed.

`hde-settings --deps [build|runtime|test]` prints the same lists *from inside HDE*, for the system it runs on: on Arch
the command is `sudo pacman -S` (not `install`, that is apt/dnf/zypper), on Debian/Ubuntu/Hyggshi OS `apt`, on Fedora
`dnf`, on openSUSE `zypper`.

## Arch package names

Arch has no separate development packages: the library package *is* the one with the headers, so `-dev` / `-devel`
disappears from every name. Otherwise the same list.

| Debian / Ubuntu              | Arch                        |
|------------------------------|-----------------------------|
| `build-essential`            | `base-devel`                |
| `pkg-config`                 | `pkgconf`                   |
| `libgtk-3-dev`               | `gtk3`                      |
| `libwnck-3-dev`              | `libwnck3`                  |
| `libgtk-layer-shell-dev`     | `gtk-layer-shell`           |
| `wayland-protocols`          | `wayland-protocols`         |
| `libxi-dev`                  | `libxi`                     |
| `libxrandr-dev`              | `libxrandr`                 |
| `libx11-dev`                 | `libx11`                    |
| `libxfixes-dev`              | `libxfixes`                 |
| `libxcomposite-dev`          | `libxcomposite`             |
| `libxcursor-dev`             | `libxcursor`                |
| `libxkbcommon-dev`           | `libxkbcommon`              |
| `libxkbcommon-x11-dev`       | `libxkbcommon-x11`          |
| `libinput-dev`               | `libinput`                  |
| `libpixman-1-dev`            | `pixman`                    |
| `libcairo2-dev`              | `cairo`                     |
| `libpam0g-dev`               | `pam` (PAM is in `base`)    |
| `libxcb1-dev`                | `libxcb`                    |
| `libwlroots-dev`             | `wlroots` (rolling: `wlroots0.18` exists side by side when needed) |
| `libvte-2.91-dev` (optional) | `vte3`                      |
| `metacity` / `openbox`       | `metacity` / `openbox`      |
| `x11-xserver-utils`          | `xorg-xset xorg-xsetroot xorg-xrandr` |
| `x11-utils`                  | `xorg-xprop xorg-xwininfo`  |
| `xserver-xorg-video-dummy`   | `xf86-video-dummy`          |
| `xserver-xephyr`             | `xorg-server-xephyr`        |
| `python3-dbusmock`           | `python-dbusmock`           |
| `libnotify-bin`              | `libnotify`                 |
| `imagemagick`                | `imagemagick`               |
| `network-manager`            | `networkmanager`            |
| `pipewire-pulse`             | `pipewire-pulse`            |
| `pulseaudio-utils`           | `libpulse`                  |

## NexWM's Wayland compositor and the rolling wlroots

Arch is rolling, so it carries the newest wlroots (0.19, 0.20, 0.21 …). `nexwm/src/wayland.c` supports 0.17 and newer
(`-DNEXWM_WLROOTS_MINOR`, chosen in the Makefile from the version `pkg-config` reports) and the CI job builds the
compositor against Arch's current wlroots, so a wlroots release that breaks the source is found there first — and if
Arch ever lands a release the source cannot support, `wlroots0.18` from the AUR keeps the compositor buildable.

## Test it

```sh
sh packaging/arch/deps.sh install test          # Xvfb, xdotool, ImageMagick, python-dbusmock, ...
make -j"$(nproc)"
make check                                      # the whole HDE smoke test in Xvfb (no X server of yours needed)
make check-login                                # the SDDM login screen
```

The CI job (`Arch Linux (pacman, current wlroots)`) runs exactly this: install through `packaging/arch/deps.sh`, build,
`make check`, and the unit tests — the same smoke test every other job runs, so the pictures of the desktop, the panel
and the Settings windows of an Arch build end up in the same artifact (`hde-arch`, on GitHub: the run → *Artifacts*).
