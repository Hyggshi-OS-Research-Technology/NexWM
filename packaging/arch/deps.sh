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
# An entry "a|b|c" means: the first of these pacman has. The alternatives are for the packages other Arch-based
# systems (Artix, Manjaro) name differently — and for the ones Arch keeps only in numbered flavours, wlroots above
# all: Arch dropped the plain 'wlroots' for wlroots0.18 / 0.19 / 0.20 and retires the oldest of them as a new series
# comes out, so a list written down here goes stale. `pick` therefore asks pacman which of them it really has
# before it gives up (see pacman_newest below).
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
libxext
libxcb
xcb-util-wm
wlroots0.21|wlroots0.20|wlroots0.19|wlroots0.18|wlroots0.17|wlroots
vte3
"

RUNTIME_MIN="
metacity|marco|xfwm4|openbox
dbus
pam
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
udisks2
gnome-keyring
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

# `install` on a fresh Arch container needs a full sync and upgrade before adding packages. Syncing only the
# databases (`-Sy`) and then installing packages can leave the base system partially upgraded. An installed system
# with sync databases already present is left alone.
SUDO=""
[ "$(id -u)" = 0 ] || SUDO="sudo"
if [ "$action" = install ] && [ -z "$(ls -A /var/lib/pacman/sync 2>/dev/null)" ]; then
    echo "note: no package information yet (a fresh container): running 'pacman -Syu' first" >&2
    $SUDO pacman -Syu --noconfirm
fi

# pacman knows this package (it is in the sync databases)
available() {
    pacman -Si "$1" >/dev/null 2>&1
}
installed() {
    pacman -Qq "$1" >/dev/null 2>&1
}
# Arch only keeps a few numbered flavours of a package such as wlroots and drops the oldest when a new series comes
# out, so none of the names written in the lists above may be in the databases any more even though the package is
# alive and well. Ask pacman which numbered ones it has and take the newest: that is the one a compositor is built
# against. Prints a note and fails when pacman has none at all, so this never hides a name that is simply wrong.
pacman_newest() {
    p_base=$(printf '%s\n' "$1" | cut -d'|' -f1 | sed 's/[0-9][0-9.]*$//')
    [ -n "$p_base" ] || return 1
    p_best=$(pacman -Ssq "^${p_base}[0-9]" 2>/dev/null | sort -V | tail -n 1)
    [ -n "$p_best" ] || return 1
    echo "note: none of '$1' is in the databases, but pacman has '$p_best': using that" >&2
    echo "$p_best"
}

# the first alternative that exists here. If the sync databases are present but none match, try pacman_newest, and
# only when that finds nothing stop with a useful error instead of passing a stale package name to pacman. With no
# databases, retain the first-choice fallback.
pick() {
    p_ifs=$IFS; IFS='|'
    for cand in $1; do
        if available "$cand"; then IFS=$p_ifs; echo "$cand"; return 0; fi
    done
    IFS=$p_ifs
    if [ -n "$(ls -A /var/lib/pacman/sync 2>/dev/null)" ]; then
        if p_newest=$(pacman_newest "$1"); then
            echo "$p_newest"
            return 0
        fi
        echo "$0: none of '$1' is available in the pacman sync databases" >&2
        return 1
    fi
    first=$(printf '%s\n' "$1" | cut -d'|' -f1)
    echo "note: pacman has no package information yet (pacman -Syu): using '$first'" >&2
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

echo "$SUDO pacman -S --needed --noconfirm$names" >&2
# shellcheck disable=SC2086  # a list of package names
$SUDO pacman -S --needed --noconfirm $names
