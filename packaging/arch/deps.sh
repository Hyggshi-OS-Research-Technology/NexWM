#!/bin/sh
# packaging/arch/deps.sh — the Arch Linux (pacman) dependencies of HDE, in one place: the CI job (.github/workflows/build.yml),
# packaging/arch/README.md and anyone building HDE on Arch use the same list.
#
#   packaging/arch/deps.sh list GROUP...       print the packages, one per line
#   packaging/arch/deps.sh install GROUP...    pacman -S them (as root; with sudo when you are not)
#   packaging/arch/deps.sh missing GROUP...    print the ones that are not installed
#   packaging/arch/deps.sh groups              the names of the groups
#
# Groups:
#   build            to compile HDE (GTK 3, libwnck, X11, layer-shell, Wayland, XCB, wlroots, cairo, xkbcommon, PAM)
#   runtime-minimal  what the session needs to start (a window manager, D-Bus, X tools, icons)
#   runtime-full     what HDE's features use when present (Wi-Fi, sound, power, Wayland session, screenshots, SDDM)
#   test             the test suite (Xvfb, xdotool, ImageMagick, dbusmock, ...)
#   wayland-session  the "HDE (Wayland)" session: labwc, grim, slurp, wtype, Xwayland
#
# An entry "a|b|c" means: the first of these pacman has. Arch has one rolling repository (no wlroots0.18 next to
# wlroots), so the alternatives are mostly for the window manager and for the packages other Arch-based systems
# (Artix, Manjaro) name differently.
#
# Arch names its *development* packages like the libraries they are (gtk3, libwnck3, libxcb, pam, wlroots: no -dev,
# no -devel), which is the one thing to remember coming from Debian or Fedora: `hde-settings --deps build` prints the
# same list translated for the system it runs on.
set -eu

# ---------------------------------------------------------------- the lists
BUILD="
base-devel
pkgconf
gtk3
libwnck3
libxi
libxrandr
libx11
libxfixes
libxcomposite
libxcursor
gtk-layer-shell
wayland
wayland-protocols
libxkbcommon
libxkbcommon-x11
libinput
pixman
cairo
pam
libxcb
xcb-util-wm
wlroots
vte3
"

RUNTIME_MIN="
metacity|marco|xfwm4|openbox
dbus
xorg-xset
xorg-xsetroot
xorg-xrandr
adwaita-icon-theme
librsvg
glib2
"

RUNTIME_FULL="
networkmanager
bluez
libpulse
pipewire-pulse|pulseaudio
wireplumber
power-profiles-daemon
upower
polkit-gnome
gnome-themes-extra
libnotify
playerctl
ddcutil
xdg-desktop-portal-gtk
xorg-xwayland
labwc
grim
slurp
sddm
"

TEST="
xorg-server-xvfb
xorg-xinput
xdotool
diffutils
xterm
xorg-xprop
xorg-xwininfo
dbus
python
python-dbusmock
imagemagick
zenity
procps-ng
which
ttf-dejavu
wtype
"

WAYLAND_SESSION="
labwc
grim
slurp
wtype
xorg-xwayland
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
[ "$# " != "0 " ] || usage
# `list` only prints the names (it works anywhere, e.g. to write a PKGBUILD); the others need pacman
if [ "$action" != list ]; then
    command -v pacman >/dev/null 2>&1 || { echo "$0: pacman not found: this script is for Arch Linux" >&2; exit 1; }
fi

# pacman knows this package (it is in the sync databases: -Sy has been run)
available() {
    pacman -Si "$1" >/dev/null 2>&1
}
installed() {
    pacman -Qq "$1" >/dev/null 2>&1
}
# the first alternative that exists here. When pacman has no sync database (a machine that never ran -Sy, or a
# container that has just unpacked one), the first alternative is used: an Arch without a window manager would be a
# session that does not start, which is worse than a package name that needs a second look.
pick() {
    p_ifs=$IFS; IFS='|'
    for cand in $1; do
        if available "$cand"; then IFS=$p_ifs; echo "$cand"; return 0; fi
    done
    IFS=$p_ifs
    first=$(printf '%s\n' "$1" | cut -d'|' -f1)
    echo "note: pacman has no package information yet (pacman -Sy): using '$first'" >&2
    echo "$first"
    return 0
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
            names="$names $(pick "$entry")"
            ;;
        esac
    done <<EOF
$(group_entries "$g")
EOF
done

[ "$action" = install ] || exit 0
[ -n "${names# }" ] || { echo "$0: nothing to install" >&2; exit 0; }

SUDO=""
[ "$(id -u)" = 0 ] || SUDO="sudo"
echo "$SUDO pacman -S --needed --noconfirm$names" >&2
# shellcheck disable=SC2086  # a list of package names
$SUDO pacman -S --needed --noconfirm $names
