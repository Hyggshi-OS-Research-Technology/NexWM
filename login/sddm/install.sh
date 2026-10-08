#!/bin/sh
# login/sddm/install.sh — put the HDE login screen (the SDDM theme of this repository, login/sddm/hde) in place.
#
# It is installed as 'hde-login' by 'make install' ('sudo hde-login') and can be run from the checkout as
# 'sudo sh login/sddm/install.sh'.
#
#   sudo sh login/sddm/install.sh                    install the theme and make SDDM use it
#   sudo sh login/sddm/install.sh --default-session   ... and preselect HDE as the session SDDM starts
#   sudo sh login/sddm/install.sh --no-config         only the theme files (leave SDDM's configuration alone)
#   sudo sh login/sddm/install.sh --root DIR          install under DIR as if it were /  (packaging, tests, ISOs)
#   sudo sh login/sddm/install.sh --dir DIR           install the theme into DIR (default /usr/share/sddm/themes)
#        sh login/sddm/install.sh --dry-run           say what would happen, change nothing
#   sudo sh login/sddm/install.sh --uninstall         remove the theme and the configuration this script wrote
#
# What "make SDDM use it" means: /etc/sddm.conf.d/50-hde-theme.conf with [Theme] Current=hde (Debian, Fedora,
# openSUSE and Arch all read that directory; /etc wins over the distribution's own files in /usr/lib/sddm/sddm.conf.d),
# and — with --default-session — [Users] DefaultSession=hde.desktop (SDDM 0.20 and newer) plus state.conf's [Last]
# Session, which is what older SDDM versions preselect.
#
# The greeter this theme runs in is chosen by metadata.desktop's QtVersion: this script rewrites it for the greeters
# that are installed here (5: sddm-greeter, 6: sddm-greeter-qt6), so the same theme works on Debian (Qt 5) and on
# Fedora (Qt 6) without the user having to know which one their SDDM was built for.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
THEME_NAME=${HDE_SDDM_THEME_NAME:-hde}
SESSION=${HDE_SDDM_SESSION:-hde.desktop}

ROOT=${HDE_SDDM_ROOT:-}
THEME_DIR_DEFAULT=/usr/share/sddm/themes
THEME_DIR=""
DO_CONFIG=1
DO_SESSION=0
DRY=0
UNINSTALL=0

say()  { printf '%s\n' "$*"; }
run()  { if [ "$DRY" = 1 ]; then say "would run: $*"; else "$@"; fi; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# how to install a package here (Debian's apt, Fedora's dnf, openSUSE's zypper, Arch's pacman)
install_hint() {
    if command -v dnf >/dev/null 2>&1; then echo "sudo dnf install $1"
    elif command -v apt >/dev/null 2>&1; then echo "sudo apt install $1"
    elif command -v zypper >/dev/null 2>&1; then echo "sudo zypper install $1"
    elif command -v pacman >/dev/null 2>&1; then echo "sudo pacman -S $1"
    else echo "install $1"; fi
}

usage() {
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
    --default-session) DO_SESSION=1 ;;
    --no-config)       DO_CONFIG=0 ;;
    --dir)             shift; THEME_DIR=${1:-} ;;
    --dir=*)           THEME_DIR=${1#*=} ;;
    --root)            shift; ROOT=${1:-} ;;
    --root=*)          ROOT=${1#*=} ;;
    --dry-run)         DRY=1 ;;
    --uninstall)       UNINSTALL=1 ;;
    -h|--help)         usage 0 ;;
    *)                 say "unknown option: $1" >&2; usage 2 ;;
    esac
    shift
done

[ -n "$THEME_DIR" ] || THEME_DIR=$THEME_DIR_DEFAULT
# --root DIR: everything happens under DIR (DIR/usr/share/sddm/themes/hde, DIR/etc/sddm.conf.d/...)
DEST_THEME="$(printf '%s%s/%s' "$ROOT" "$THEME_DIR" "$THEME_NAME")"
DEST_CONF="$(printf '%s/etc/sddm.conf.d' "$ROOT")"
DEST_CONF_FILE="$DEST_CONF/50-hde-theme.conf"

# where the theme files are: asked for (HDE_SDDM_THEME_SRC), next to this script (a checkout: login/sddm/hde), or
# already installed (this script installed as hde-login, with the theme in the theme directory)
THEME_SRC=${HDE_SDDM_THEME_SRC:-}
if [ -z "$THEME_SRC" ] && [ -f "$HERE/$THEME_NAME/Main.qml" ]; then THEME_SRC="$HERE/$THEME_NAME"; fi
if [ -z "$THEME_SRC" ] && [ -f "$DEST_THEME/Main.qml" ]; then THEME_SRC="$DEST_THEME"; fi
[ -n "$THEME_SRC" ] || THEME_SRC="$HERE/$THEME_NAME"

# ---------------------------------------------------------------- which greeter (Qt 5 or Qt 6) does this system have?
greeter_qt_version() {
    if command -v sddm-greeter >/dev/null 2>&1; then echo 5; return; fi
    if command -v sddm-greeter-qt6 >/dev/null 2>&1; then echo 6; return; fi
    if command -v sddm-greeter-qt5 >/dev/null 2>&1; then echo 5; return; fi
    echo ""     # SDDM not installed here (or only the daemon): keep what metadata.desktop says
}

# ---------------------------------------------------------------- uninstall
if [ "$UNINSTALL" = 1 ]; then
    [ "$(id -u)" = 0 ] || [ -n "$ROOT" ] || die "run it as root to remove the theme from $THEME_DIR (or use --root DIR)"
    say "removing $DEST_THEME"
    run rm -rf "$DEST_THEME"
    if [ -f "$DEST_CONF_FILE" ] && grep -q "^# HDE login screen" "$DEST_CONF_FILE" 2>/dev/null; then
        say "removing $DEST_CONF_FILE (written by this script)"
        run rm -f "$DEST_CONF_FILE"
    elif [ -e "$DEST_CONF_FILE" ]; then
        say "keeping $DEST_CONF_FILE: it was not written by this script"
    fi
    say "done. SDDM goes back to its default theme after a restart of the login screen (sudo systemctl restart sddm)"
    exit 0
fi

# ---------------------------------------------------------------- the theme has to be there
[ -f "$THEME_SRC/Main.qml" ] || die "the theme is not in $THEME_SRC (a checkout has it in login/sddm/$THEME_NAME; \
'make install' puts it in /usr/share/sddm/themes/$THEME_NAME, or set HDE_SDDM_THEME_SRC)"
[ -f "$THEME_SRC/theme.conf" ] || die "$THEME_SRC/theme.conf not found"
[ -f "$THEME_SRC/metadata.desktop" ] || die "$THEME_SRC/metadata.desktop not found"

if [ -z "$ROOT" ] && [ "$(id -u)" != 0 ]; then
    die "run it as root: sudo sh $0 ...   (or --root DIR to install into another root, --dry-run to only look)"
fi

QT=$(greeter_qt_version)
say "HDE login screen"
say "  theme:      $THEME_SRC -> $DEST_THEME"
if [ -n "$QT" ]; then say "  greeter:    Qt $QT ($(command -v sddm-greeter-qt6 || command -v sddm-greeter))"
else say "  greeter:    SDDM is not installed here (the theme keeps QtVersion=$(sed -n 's/^QtVersion=//p' "$THEME_SRC/metadata.desktop" | head -n1)); $(install_hint sddm)"; fi
[ "$DO_CONFIG" = 1 ] && say "  config:     $DEST_CONF_FILE  ([Theme] Current=$THEME_NAME)"
[ "$DO_SESSION" = 1 ] && say "  session:    HDE preselected ([Users] DefaultSession=$SESSION, and state.conf for older SDDM)"

# ---------------------------------------------------------------- copy the theme
if [ "$THEME_SRC" = "$DEST_THEME" ]; then
    say "  theme:      already in place ($DEST_THEME): only the configuration is touched"
else
run mkdir -p "$DEST_THEME"
# the files of the theme, and only them: *.qml, components/, assets/, theme.conf, metadata.desktop
for f in Main.qml theme.conf metadata.desktop; do
    [ -f "$THEME_SRC/$f" ] || continue
    run cp -f "$THEME_SRC/$f" "$DEST_THEME/$f"
done
for d in components assets; do
    [ -d "$THEME_SRC/$d" ] || continue
    run rm -rf "$DEST_THEME/$d"
    run cp -r "$THEME_SRC/$d" "$DEST_THEME/$d"
done
# ... and the translations, when there are some
if [ -d "$THEME_SRC/translations" ]; then
    run rm -rf "$DEST_THEME/translations"
    run cp -r "$THEME_SRC/translations" "$DEST_THEME/translations"
fi
run chmod -R a+rX "$DEST_THEME"
fi

# the greeter of this system
if [ -n "$QT" ] && [ "$DRY" != 1 ]; then
    # in-place when sed can (GNU sed: -i), otherwise through a temporary file. Not `a || b && c`: in sh that is
    # `(a || b) && c`, so the rename would also run after a *successful* in-place edit (this made the installer
    # stop with "mv: cannot stat ...metadata.desktop.new" on the CI machines)
    if ! sed -i "s/^QtVersion=.*/QtVersion=$QT/" "$DEST_THEME/metadata.desktop" 2>/dev/null; then
        sed "s/^QtVersion=.*/QtVersion=$QT/" "$DEST_THEME/metadata.desktop" > "$DEST_THEME/metadata.desktop.new" &&
            mv "$DEST_THEME/metadata.desktop.new" "$DEST_THEME/metadata.desktop"
    fi
    say "  metadata:   QtVersion=$QT (the greeter installed here)"
fi

# ---------------------------------------------------------------- SDDM's configuration
if [ "$DO_CONFIG" = 1 ]; then
    run mkdir -p "$DEST_CONF"
    SESSION_LINE=""
    [ "$DO_SESSION" = 1 ] && SESSION_LINE="[Users]
DefaultSession=$SESSION
"
    run sh -c "cat > '$DEST_CONF_FILE' <<EOF
# HDE login screen (Hyggshi Desktop Environment). Written by login/sddm/install.sh of
# https://github.com/Hyggshi-OS-Research-Technology/NexWM — remove this file to go back to the theme SDDM had.
[Theme]
Current=$THEME_NAME
${SESSION_LINE}EOF"
    say "  wrote:      $DEST_CONF_FILE"
fi

# older SDDM preselects the session of state.conf (SDDM overwrites it after every login, so it is only a first choice)
if [ "$DO_SESSION" = 1 ] && [ -z "$ROOT" ]; then
    STATE=""
    if command -v getent >/dev/null 2>&1; then
        HOME_SDDM=$(getent passwd sddm 2>/dev/null | cut -d: -f6 || true)
        [ -n "$HOME_SDDM" ] && STATE="$HOME_SDDM/state.conf"
    fi
    [ -n "$STATE" ] || STATE=/var/lib/sddm/state.conf
    if [ -d "$(dirname "$STATE")" ]; then
        if [ "$DRY" = 1 ]; then
            say "would write:  [Last] Session=$SESSION in $STATE"
        elif [ -f "$STATE" ] && grep -q '^\[Last\]' "$STATE"; then
            if grep -q '^Session=' "$STATE"; then
                sed -i "s/^Session=.*/Session=$SESSION/" "$STATE"
            else
                sed -i "/^\[Last\]/a Session=$SESSION" "$STATE"
            fi
            say "  session:    $SESSION in $STATE (older SDDM)"
        else
            printf '[Last]\nSession=%s\n' "$SESSION" > "$STATE"
            say "  session:    $SESSION in $STATE (older SDDM)"
        fi
    fi
fi

# ---------------------------------------------------------------- what happens next
cat <<EOF

Done. What is left:
  * if SDDM is not the login screen yet:  sudo systemctl enable --now sddm
  * to see this login screen right now:   sudo systemctl restart sddm   (log out first!)
  * to try it in a window, without logging out:
EOF
if command -v sddm-greeter-qt6 >/dev/null 2>&1; then
    echo "      sddm-greeter-qt6 --test --theme $DEST_THEME"
elif command -v sddm-greeter >/dev/null 2>&1; then
    echo "      sddm-greeter --test --theme $DEST_THEME"
else
    echo "      ($(install_hint sddm), then: sddm-greeter --test --theme $DEST_THEME)"
fi
cat <<EOF
  * the look of the screen (background, accent colour, clock, user list, ...): $DEST_THEME/theme.conf
    — or a copy of it with only the lines to change, $DEST_THEME/theme.conf.user, which survives an update
  * this script again with --uninstall removes the theme and the configuration it wrote
EOF
