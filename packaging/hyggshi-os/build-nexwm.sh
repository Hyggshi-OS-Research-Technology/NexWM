#!/bin/bash
# build-nexwm.sh — build HDE (the Hyggshi Desktop Environment, this repository; formerly NexDE / NexWM) from source and
# install it into a Debian rootfs: the "HDE" (X11) and "HDE (Wayland)" sessions on the login screen.
#
# A drop-in replacement for scripts/build-nexwm.sh of Hyggshi-OS-Research-Technology/Hyggshi-OS, which still builds
# the old NexDE (xcb + Qt 6, bin/nexwm, nex-panel, start-nexde) that this repository no longer contains. Same
# interface: run it as root INSIDE the chroot of the ISO (like desktop.sh), with the same variables:
#   NEXWM_REPO_URL   git URL of this repository   (default: https://github.com/Hyggshi-OS-Research-Technology/NexWM.git)
#   NEXWM_REF        branch or tag to build       (default: main)
#   SRC_DIR          where the source is cloned   (default: /tmp/nexwm-src)
#   PREFIX           install prefix               (default: /usr, like the .deb packages of the ISO)
#   DEBUG_MODE=true  set -x
# and a few new ones:
#   NEXWM_LOCAL_SRC=DIR     build this checkout instead of cloning (CI)
#   HDE_RUNTIME=minimal     only what HDE needs to start (window manager, D-Bus, icons); default "full" adds what its
#                           features use when present: NetworkManager (Wi-Fi), PulseAudio/PipeWire tools (volume),
#                           power-profiles-daemon (power mode), labwc + grim + slurp (Wayland session), polkit agent,
#                           ddcutil (brightness of desktop monitors)
#   HDE_DEFAULT_SESSION=true   make HDE the default session of LightDM / SDDM
#   KEEP_BUILD_DEPS=true    keep the compiler and the -dev packages (smaller ISOs remove them: the default)
#
# The session files come from `make install` (Type=Application with absolute Exec/TryExec paths, which LightDM, SDDM
# and GDM all accept): /usr/share/xsessions/hde.desktop and /usr/share/wayland-sessions/hde-wayland.desktop. The
# nexwm.desktop / start-nexde of the old script are removed: they start programs that no longer exist.
set -e
[ "${DEBUG_MODE:-}" = "true" ] && set -x
export DEBIAN_FRONTEND=noninteractive

: "${NEXWM_REPO_URL:=https://github.com/Hyggshi-OS-Research-Technology/NexWM.git}"
: "${NEXWM_REF:=main}"
: "${SRC_DIR:=/tmp/nexwm-src}"
: "${PREFIX:=/usr}"
: "${HDE_RUNTIME:=full}"

say() { printf '===== %s =====\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run it as root (inside the chroot of the ISO)"
command -v apt-get >/dev/null 2>&1 || die "apt-get not found: this script is for Debian-based systems"

BUILD_DEPS="git ca-certificates build-essential pkg-config libgtk-3-dev libwnck-3-dev libxi-dev libxrandr-dev libx11-dev
            libgtk-layer-shell-dev libwayland-dev"
# what HDE needs to start: a GTK window manager (title bars follow the theme and Dark mode), D-Bus, X tools used by the
# session (xset: screen blanking, xsetroot: the pointer), icons and SVG support
RUNTIME_MIN="metacity dbus dbus-x11 x11-xserver-utils adwaita-icon-theme librsvg2-common libglib2.0-bin"
# what its features use when present
RUNTIME_FULL="network-manager pulseaudio-utils power-profiles-daemon labwc xwayland grim slurp xdg-desktop-portal-gtk
              upower ddcutil libnotify-bin"
# one polkit authentication agent (the password window of "Install updates", disks, ...): the first one there is
POLKIT_AGENTS="polkit-gnome policykit-1-gnome lxpolkit mate-polkit"

installed() { dpkg-query -W -f='${Status}\n' "$1" 2>/dev/null | grep -q "install ok installed"; }
available() { apt-cache show "$1" >/dev/null 2>&1; }

say "packages"
apt-get update
NEW_BUILD_DEPS=""
for p in $BUILD_DEPS; do installed "$p" || NEW_BUILD_DEPS="$NEW_BUILD_DEPS $p"; done
RUNTIME="$RUNTIME_MIN"
if [ "$HDE_RUNTIME" != "minimal" ]; then
    RUNTIME="$RUNTIME $RUNTIME_FULL"
    for a in $POLKIT_AGENTS; do
        if available "$a"; then RUNTIME="$RUNTIME $a"; break; fi
    done
fi
WANT=""
for p in $BUILD_DEPS $RUNTIME; do
    if available "$p"; then WANT="$WANT $p"
    else echo "note: $p is not available here: skipped"
    fi
done
# shellcheck disable=SC2086  # a list of package names
apt-get install -y --no-install-recommends $WANT

if [ -n "${NEXWM_LOCAL_SRC:-}" ]; then
    say "source: $NEXWM_LOCAL_SRC"
    [ -f "$NEXWM_LOCAL_SRC/Makefile" ] || die "$NEXWM_LOCAL_SRC is not a checkout of HDE (no Makefile)"
    SRC="$NEXWM_LOCAL_SRC"
else
    say "clone $NEXWM_REPO_URL @ $NEXWM_REF"
    rm -rf "$SRC_DIR"
    git clone --branch "$NEXWM_REF" --depth=1 "$NEXWM_REPO_URL" "$SRC_DIR" ||
        die "clone failed (wrong NEXWM_REPO_URL / NEXWM_REF, or no network): '$NEXWM_REPO_URL' '$NEXWM_REF'"
    SRC="$SRC_DIR"
fi

say "make"
make -C "$SRC" -j"$(nproc)" all
PROGRAMS="hde-session hde-desktop hde-panel hde-settings hde-hotkeys hde-xsettings hde-screenshot"
for p in $PROGRAMS; do
    [ -x "$SRC/build/$p" ] || die "build/$p was not built (see the make output above)"
done

say "make install PREFIX=$PREFIX"
make -C "$SRC" install PREFIX="$PREFIX"
for p in $PROGRAMS hde-start; do
    [ -x "$PREFIX/bin/$p" ] || die "$PREFIX/bin/$p is missing after make install"
done
for f in /usr/share/xsessions/hde.desktop /usr/share/wayland-sessions/hde-wayland.desktop; do
    [ -f "$f" ] || die "$f is missing after make install"
    grep -q "^Exec=$PREFIX/bin/hde-start" "$f" || die "$f does not start $PREFIX/bin/hde-start"
done

# the session of the old NexDE script: its programs no longer exist, the login screen would offer a broken session
for f in /usr/share/xsessions/nexwm.desktop "$PREFIX/bin/start-nexde"; do
    if [ -e "$f" ]; then echo "removing $f (old NexDE session)"; rm -f "$f"; fi
done

if [ "${HDE_DEFAULT_SESSION:-}" = "true" ]; then
    say "HDE as the default session"
    if [ -d /etc/lightdm ]; then
        mkdir -p /etc/lightdm/lightdm.conf.d
        printf '[Seat:*]\nuser-session=hde\n' > /etc/lightdm/lightdm.conf.d/50-hde.conf
    fi
    if [ -d /etc/sddm.conf.d ] || command -v sddm >/dev/null 2>&1; then
        mkdir -p /etc/sddm.conf.d
        printf '[Autologin]\nSession=hde.desktop\n' > /etc/sddm.conf.d/50-hde.conf
    fi
fi

# keep every library the programs use: they came in as dependencies of the -dev packages, which go away below
say "runtime libraries"
LIBS=$(for b in "$PREFIX"/bin/hde-*; do ldd "$b" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// { print $3 }'; done | sort -u)
PKGS=""
for l in $LIBS; do
    p=$( { dpkg -S "$l" 2>/dev/null || dpkg -S "$(readlink -f "$l")" 2>/dev/null || true; } | head -n 1)
    p=${p%%:*}
    [ -n "$p" ] && case " $PKGS " in *" $p "*) ;; *) PKGS="$PKGS $p" ;; esac
done
# shellcheck disable=SC2086
[ -n "$PKGS" ] && apt-mark manual $PKGS >/dev/null
echo "kept:$PKGS"

if [ "${KEEP_BUILD_DEPS:-}" != "true" ] && [ -n "$NEW_BUILD_DEPS" ]; then
    say "remove the build tools installed by this script"
    # shellcheck disable=SC2086
    apt-get purge -y --autoremove $NEW_BUILD_DEPS
fi
[ -z "${NEXWM_LOCAL_SRC:-}" ] && rm -rf "$SRC_DIR"

say "check"
missing=$(for b in "$PREFIX"/bin/hde-*; do ldd "$b" 2>/dev/null | grep "not found" | sed "s|^|$b: |"; done)
[ -z "$missing" ] || die "libraries missing after the build tools were removed:
$missing"
"$PREFIX/bin/hde-settings" --version
echo "OK: HDE is installed in $PREFIX/bin; sessions: /usr/share/xsessions/hde.desktop (HDE)," \
     "/usr/share/wayland-sessions/hde-wayland.desktop (HDE (Wayland), with labwc)"
say "build-nexwm.sh done"
