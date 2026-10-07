# HDE in the Hyggshi OS ISO

`build-nexwm.sh` builds HDE from this repository and installs it into the root filesystem of the ISO, with the
**HDE** (X11) and **HDE (Wayland)** sessions on the login screen. It replaces `scripts/build-nexwm.sh` of
[Hyggshi-OS](https://github.com/Hyggshi-OS-Research-Technology/Hyggshi-OS), which still builds the old NexDE (xcb,
Qt 6, `bin/nexwm`, `start-nexde`) that this repository no longer contains — with `BUILD_NEXWM=true` that script stops
at its "missing bin/nexwm" check.

It keeps the same interface, so the workflow step (`Build-Hyggshi-OS-ISO.yml`, *[build-nexwm.sh] Build & install
NexWM from source*) does not change: copy this file over `scripts/build-nexwm.sh` of the Hyggshi-OS repository, or let
that step download it:

```sh
curl -fsSL https://raw.githubusercontent.com/Hyggshi-OS-Research-Technology/NexWM/main/packaging/hyggshi-os/build-nexwm.sh \
    -o scripts/build-nexwm.sh
```

Run as root inside the chroot:

| Variable | Default | |
|---|---|---|
| `NEXWM_REPO_URL` | `https://github.com/Hyggshi-OS-Research-Technology/NexWM.git` | the repository |
| `NEXWM_REF` | `main` | branch or tag |
| `SRC_DIR` | `/tmp/nexwm-src` | where it is cloned (removed afterwards) |
| `PREFIX` | `/usr` | install prefix |
| `DEBUG_MODE` | | `true`: `set -x` |
| `NEXWM_LOCAL_SRC` | | build this checkout instead of cloning |
| `HDE_RUNTIME` | `full` | `minimal`: only what HDE needs to start (Metacity, D-Bus, icons); `full` also installs what its features use (NetworkManager, PulseAudio tools, power-profiles-daemon, labwc + grim + slurp for the Wayland session, a polkit agent, UPower, ddcutil) |
| `HDE_DEFAULT_SESSION` | | `true`: HDE is the default session of LightDM / SDDM |
| `KEEP_BUILD_DEPS` | | `true`: keep the compiler and the `-dev` packages |

What it does: installs the build dependencies and the runtime packages, builds (`make`), checks that all seven programs
were built, `make install PREFIX=/usr` (programs, `hde-start`, `/usr/share/xsessions/hde.desktop`,
`/usr/share/wayland-sessions/hde-wayland.desktop`, logos, the portal configuration), removes the `nexwm.desktop` /
`start-nexde` of the old script, marks every library the programs use as manually installed and then removes only the
build tools it installed itself, and finally checks that no library is missing.

The CI of this repository runs it in a `debian:trixie` container (job *Hyggshi OS install script*) and starts the
installed session in Xvfb.
