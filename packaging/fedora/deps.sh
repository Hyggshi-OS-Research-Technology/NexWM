#!/bin/sh
# packaging/fedora/deps.sh — the Fedora (dnf) dependencies of HDE, in one place: the CI jobs (.github/workflows/build.yml),
# packaging/fedora/README.md and anyone building HDE on Fedora use the same list.
#
#   packaging/fedora/deps.sh list GROUP...       print the packages, one per line
#   packaging/fedora/deps.sh install GROUP...    dnf install them (skipping what this Fedora does not have)
#   packaging/fedora/deps.sh missing GROUP...    print the ones that are not installed
#   packaging/fedora/deps.sh groups              the names of the groups
#
# Groups:
#   build            to compile HDE (GTK 3, libwnck, X11, layer-shell, Wayland, XCB, wlroots, cairo, xkbcommon)
#   runtime-minimal  what the session needs to start (a window manager, D-Bus, X tools, icons)
#   runtime-full     what HDE's features use when present (Wi-Fi, sound, power, Wayland session, screenshots)
#   test             the test suite (Xvfb, xdotool, ImageMagick, dbusmock, ...)
#   wayland-session  the "HDE (Wayland)" session: labwc, grim, slurp (also in runtime-full)
#
# An entry "a|b|c" means: the first of these Fedora has. Fedora renames packages between releases (wlroots, the X tools
# that used to be xorg-x11-server-utils, ...), so the alternatives are what makes the same list work on Fedora 41, 42
# and Rawhide.
set -eu

# ---------------------------------------------------------------- the lists
BUILD="
gcc
make
pkgconf-pkg-config
gtk3-devel
libwnck3-devel
libXi-devel
libXrandr-devel
libX11-devel
libXfixes-devel
libXcomposite-devel
libXcursor-devel
gtk-layer-shell-devel
wayland-devel
libxkbcommon-devel
libxkbcommon-x11-devel
libinput-devel
pixman-devel
cairo-devel
libxcb-devel
xcb-util-wm-devel
wlroots-devel|wlroots0.21-devel|wlroots0.20-devel|wlroots0.19-devel|wlroots0.18-devel|wlroots0.17-devel
vte291-devel|vte291-gtk4-devel
"

RUNTIME_MIN="
metacity|marco|xfwm4|openbox
dbus-daemon
dbus-tools|dbus-x11
xset|xorg-x11-server-utils
xsetroot|xorg-x11-server-utils
xrandr|xorg-x11-server-utils
adwaita-icon-theme
librsvg2
glib2
"

RUNTIME_FULL="
NetworkManager
bluez
pipewire-pulseaudio|pulseaudio
pulseaudio-utils
wireplumber
power-profiles-daemon
upower
polkit-gnome
gnome-themes-extra
libnotify
playerctl
ddcutil
xdg-desktop-portal-gtk
xorg-x11-server-Xwayland
labwc
grim
slurp
swaylock|gtklock
sddm
"

TEST="
xorg-x11-server-Xvfb
xdotool
diffutils
xterm
xprop|xorg-x11-utils
xwininfo|xorg-x11-utils
dbus-tools|dbus-x11
python3
python3-dbusmock
ImageMagick
zenity
procps-ng
which
dejavu-sans-fonts
wtype
"

WAYLAND_SESSION="
labwc
grim
slurp
wtype
xorg-x11-server-Xwayland
"

usage() {
    echo "usage: $0 list|install|missing GROUP...        (groups: $(groups))" >&2
    exit 2
}

groups() { echo "build runtime-minimal runtime-full test wayland-session"; }

# the list of one group, one entry per line (entries with '|' are alternatives)
group_entries() {
    case "$1" in
    build)           printf '%s\n' $BUILD ;;
    runtime-minimal) printf '%s\n' $RUNTIME_MIN ;;
    runtime-full)    printf '%s\n' $RUNTIME_FULL ;;
    test)            printf '%s\n' $TEST ;;
    wayland-session) printf '%s\n' $WAYLAND_SESSION ;;
    *)               echo "note: unknown group '$1' (known: $(groups))" >&2; return 0 ;;
    esac
}

action=${1:-}
shift 2>/dev/null || true
[ -n "$action" ] || usage
case "$action" in list|install|missing) ;; groups) groups; exit 0 ;; *) usage ;; esac
[ "$#" -gt 0 ] || usage
# `list` only prints the names (it works anywhere, e.g. to write the documentation); the others need dnf
if [ "$action" != list ]; then
    command -v dnf >/dev/null 2>&1 || { echo "$0: dnf not found: this script is for Fedora" >&2; exit 1; }
fi

# dnf knows this package (a candidate exists in the enabled repositories)
available() {
    dnf -q list --available "$1" >/dev/null 2>&1
}
installed() {
    rpm -q --quiet "$1" 2>/dev/null
}
# the first alternative that exists here
pick() {
    p_ifs=$IFS; IFS='|'
    for cand in $1; do
        if available "$cand"; then IFS=$p_ifs; echo "$cand"; return 0; fi
    done
    IFS=$p_ifs
    return 1
}

names=""
for g in "$@"; do
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        case "$action" in
        list)
            echo "$entry"
            ;;
        missing)
            # for 'a|b': missing when neither is installed
            ok=0
            p_ifs=$IFS; IFS='|'
            for cand in $entry; do installed "$cand" && { ok=1; break; }; done
            IFS=$p_ifs
            [ "$ok" = 1 ] || echo "$entry"
            ;;
        install)
            if p=$(pick "$entry"); then names="$names $p"; else echo "note: $entry is not available in this Fedora: skipped" >&2; fi
            ;;
        esac
    done <<EOF
$(group_entries "$g")
EOF
done

[ "$action" = install ] || exit 0
[ -n "${names# }" ] || { echo "$0: nothing to install" >&2; exit 0; }

# shellcheck disable=SC2086  # a list of package names
if ! dnf install -y --setopt=install_weak_deps=False $names; then
    echo "$0: installing them all at once failed; trying one by one" >&2
    rc=0
    for p in $names; do dnf install -y --setopt=install_weak_deps=False "$p" || { echo "note: $p could not be installed" >&2; rc=1; }; done
    exit $rc
fi
