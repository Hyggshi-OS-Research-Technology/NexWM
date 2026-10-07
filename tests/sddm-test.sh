#!/bin/sh
# tests/sddm-test.sh — the HDE login screen (login/sddm/hde) tested as far as this machine can:
#
#   1. the theme itself, without a display: the files SDDM needs are there, metadata.desktop says what SDDM reads,
#      the QML is balanced and self-consistent, every picture and component it names exists, the theme.conf keys are
#      exactly the ones Main.qml reads, and it needs nothing but core QtQuick (no QtGraphicalEffects, no Controls, not
#      even SDDM's component module: the same theme has to run in the Qt 5 and the Qt 6 greeter).
#   2. the install script: --dry-run says what it would do, a real install into a throw-away root puts the theme in
#      usr/share/sddm/themes/hde and writes etc/sddm.conf.d/50-hde-theme.conf, --uninstall takes both away again.
#   3. the login screen for real: SDDM's greeter renders the theme in an X server, and the test looks at the log (no
#      QML error) and at the screen (the card is there, with the accent colour on it).
#
#   sh tests/sddm-test.sh                 everything this machine can do
#   sh tests/sddm-test.sh --no-render     only 1. and 2. (no display, no greeter needed)
#   sh tests/sddm-test.sh --theme DIR     another theme directory (default login/sddm/hde of this checkout)
#
# Environment: HDE_SDDM_DISPLAY (use this X display; default: start Xvfb on :79), HDE_TEST_OUT (/tmp/hde-sddm),
# HDE_SDDM_GREETER (the greeter to run, default: sddm-greeter-qt6 then sddm-greeter).
# Exit status = failures; results.txt has the PASS/FAIL lines (tests/ci-annotate.py reads it).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CASE=$(cd "$HERE/.." && pwd)
THEME="$CASE/login/sddm/hde"
OUT=${HDE_TEST_OUT:-/tmp/hde-sddm}
DISP=${HDE_SDDM_DISPLAY:-:79}
RENDER=1
XT="python3 $HERE/xtool.py"

while [ $# -gt 0 ]; do
    case "$1" in
    --no-render) RENDER=0 ;;
    --theme)     shift; THEME=${1:-} ;;
    --theme=*)   THEME=${1#*=} ;;
    -h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)           echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$THEME" ] && [ -d "$THEME" ] || { echo "theme directory '$THEME' not found" >&2; exit 2; }

mkdir -p "$OUT"
FAILS=0
pass() { echo "PASS: sddm: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: sddm: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: sddm: $*" | tee -a "$OUT/results.txt"; }
check() { d=$1; shift; if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi; }
have() { command -v "$1" >/dev/null 2>&1; }
skip() { echo "SKIP: sddm: $*" | tee -a "$OUT/results.txt"; }

info "theme: $THEME ($(find "$THEME" -name '*.qml' | wc -l) QML files, $(du -sk "$THEME" | cut -f1) KiB)"

# ===================== 1. the theme, without a display =====================
for f in Main.qml theme.conf metadata.desktop; do
    if [ -f "$THEME/$f" ]; then pass "the theme has $f"
    else fail "the theme has $f (missing)"; fi
done
nqml=$(find "$THEME/components" -name '*.qml' 2>/dev/null | wc -l)
if [ "$nqml" -ge 8 ]; then pass "the theme has its own components ($nqml files in components/)"
else fail "the theme has its own components (only $nqml in components/)"; fi
if [ -s "$THEME/assets/background.png" ]; then
    pass "the background is there ($(du -k "$THEME/assets/background.png" | cut -f1) KiB, drawn by login/sddm/tools/make-background.py)"
else
    fail "assets/background.png is missing (python3 login/sddm/tools/make-background.py)"
fi
# it really is a PNG (SDDM's Qt reads it, but a broken file would only show as an empty screen)
if head -c 8 "$THEME/assets/background.png" 2>/dev/null | od -An -tx1 | grep -q "89 50 4e 47"; then
    pass "assets/background.png is a PNG"
else
    fail "assets/background.png is not a PNG"
fi

# metadata.desktop: what SDDM reads to find the theme
MD="$THEME/metadata.desktop"
meta() { sed -n "s/^$1=//p" "$MD" | head -n1; }
check "metadata.desktop: Type=sddm-theme" test "$(meta Type)" = "sddm-theme"
check "metadata.desktop: MainScript is Main.qml (the file SDDM runs)" test "$(meta MainScript)" = "Main.qml"
check "metadata.desktop: ConfigFile is theme.conf (the file the theme reads settings from)" test "$(meta ConfigFile)" = "theme.conf"
check "metadata.desktop: Theme-API=2.0" test "$(meta Theme-API)" = "2.0"
case "$(meta QtVersion)" in
5|6) pass "metadata.desktop: QtVersion=$(meta QtVersion) (which greeter runs the theme)" ;;
*)   fail "metadata.desktop: QtVersion is missing or wrong ($(meta QtVersion))" ;;
esac
if [ -n "$(meta Name)" ] && [ -n "$(meta Description)" ]; then
    pass "metadata.desktop: '$(meta Name)' — it shows up in the SDDM theme choosers with a description"
else
    fail "metadata.desktop: Name and Description are needed to find the theme"
fi

# the QML of the theme: imports, balance, and what it refers to
for f in "$THEME"/Main.qml "$THEME"/components/*.qml; do
    [ -f "$f" ] || continue
    rel=${f#"$THEME"/}
    head -n 40 "$f" | grep -q "^import QtQuick" || { fail "$rel: no 'import QtQuick' in the first lines"; continue; }
    if grep -q "^import QtGraphicalEffects" "$f"; then
        fail "$rel: QtGraphicalEffects is gone in Qt 6 (the theme has to run in both greeters)"
    elif grep -q "^import QtQuick.Controls" "$f"; then
        fail "$rel: QtQuick.Controls is not in every greeter (draw the widgets instead, see components/)"
    elif grep -q "^import SddmComponents" "$f"; then
        fail "$rel: the theme draws its own widgets (only QtQuick), it does not need SDDM's component module"
    fi
    # every brace, bracket and parenthesis has to close (this machine has no QML engine: it catches the mistakes a
    # person makes, not the ones the engine catches - step 3 renders the theme for those)
    if ! have python3; then
        :
    elif python3 - "$f" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
src = re.sub(r"//[^\n]*", " ", src)
src = re.sub(r'"(?:\\.|[^"\\])*"', '""', src)
src = re.sub(r"'(?:\\.|[^'\\])*'", "''", src)
stack, pairs = [], {")": "(", "]": "[", "}": "{"}
for i, ch in enumerate(src):
    if ch in "([{":
        stack.append(ch)
    elif ch in ")]}":
        if not stack or stack.pop() != pairs[ch]:
            sys.exit(1)
if stack:
    sys.exit(1)
PY
    then :; else fail "$rel: brackets or braces do not balance (a missing } is the usual one)"; fi
done
if [ "$FAILS" = 0 ]; then
    if have python3; then
        pass "the QML of the theme is core QtQuick only (runs in the Qt 5 and the Qt 6 greeter) and balances"
    else
        pass "the QML of the theme is core QtQuick only (it runs in the Qt 5 and the Qt 6 greeter)"
        skip "the brackets of every QML file could not be checked: no python3 here"
    fi
fi

# every path the theme uses has to exist (a missing picture shows as an empty background, a missing component as an
# empty card: both are hard to see and easy to check)
miss=""
for ref in $(grep -rhoE '(Qt\.resolvedUrl\()?"\.\./[A-Za-z0-9_./-]+"' "$THEME"/Main.qml "$THEME"/components/*.qml 2>/dev/null |
             sed 's/.*"\.\.\///; s/"$//' | sort -u); do
    [ -e "$THEME/$ref" ] || miss="$miss $ref"
done
for ref in $(grep -hoE 'source: *"[A-Za-z0-9_./-]+"' "$THEME"/Main.qml "$THEME"/components/*.qml 2>/dev/null |
             sed 's/source: *"//; s/"$//' | sort -u); do
    case "$ref" in
    ""|*://*|/*) continue ;;
    esac
    [ -e "$THEME/$ref" ] || miss="$miss $ref"
done
if [ -z "$miss" ]; then pass "every picture and component the theme names exists"
else fail "the theme names files that do not exist:$miss"; fi

# theme.conf: the keys Main.qml reads are exactly the keys the file explains
KEYS=$(grep -oE 'cfg(String|Bool|Number)?\("[A-Za-z0-9_]+"' "$THEME/Main.qml" | sed 's/.*("//; s/"$//' | sort -u)
missing=""
for k in $KEYS; do
    grep -qE "^$k=" "$THEME/theme.conf" || missing="$missing $k"
done
if [ -z "$missing" ]; then
    pass "theme.conf has every key the login screen reads ($(echo "$KEYS" | wc -w) keys)"
else
    fail "theme.conf does not explain:$missing (Main.qml reads them)"
fi
if grep -q "^\[General\]" "$THEME/theme.conf"; then pass "theme.conf uses the [General] group (SDDM reads its keys without the group name)"
else fail "theme.conf has no [General] group"; fi
# values the theme would not understand (a colour without #, a word that is not one of the choices): the login screen
# must not come up as an empty rectangle because of a typo in theme.conf
badval=""
while IFS= read -r line; do
    key=${line%%=*}
    val=${line#*=}
    case "$key" in
    accent)         case "$val" in \#[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]) ;; *) badval="$badval  $line" ;; esac ;;
    dim)            case "$val" in [0-9]|[0-9].[0-9]*|0.[0-9]*) ;; *) badval="$badval  $line" ;; esac ;;
    cardWidth)      case "$val" in [0-9]|[0-9][0-9]|[0-9][0-9][0-9]|[0-9][0-9][0-9][0-9]) ;; *) badval="$badval  $line" ;; esac ;;
    clockPosition)  case "$val" in top-left|top-center|top-right) ;; *) badval="$badval  $line" ;; esac ;;
    userMode)       case "$val" in user|username) ;; *) badval="$badval  $line" ;; esac ;;
    buttonStyle)    case "$val" in wide|compact) ;; *) badval="$badval  $line" ;; esac ;;
    esac
done <<EOF
$(sed -n 's/^\(accent\|dim\|cardWidth\|clockPosition\|userMode\|buttonStyle\)=\(.*\)$/\1=\2/p' "$THEME/theme.conf")
EOF
if [ -z "$badval" ]; then pass "the values in theme.conf are of the right shape (colour, number, one of the choices)"
else fail "theme.conf has values the theme does not understand:"; printf '%s\n' "$badval" >> "$OUT/results.txt"; printf '%s\n' "$badval"; fi

# ===================== 2. the install script =====================
INSTALL="$CASE/login/sddm/install.sh"
if [ -x "$INSTALL" ] || [ -f "$INSTALL" ]; then
    pass "login/sddm/install.sh is there (the installer of the login screen)"
    ROOT="$OUT/root"
    rm -rf "$ROOT"
    out=$(sh "$INSTALL" --root "$ROOT" --default-session --dry-run 2>&1)
    if echo "$out" | grep -q "would run: cp -f" && echo "$out" | grep -q "$ROOT/usr/share/sddm/themes/hde"; then
        pass "--dry-run says what it would copy where, and changes nothing"
    else
        fail "--dry-run: $(echo "$out" | tr '\n' '|' | cut -c1-200)"
    fi
    [ -e "$ROOT/usr" ] && fail "--dry-run created files (it must not)" || pass "--dry-run left the file system alone"

    out=$(sh "$INSTALL" --root "$ROOT" --default-session 2>&1)
    rc=$?
    ok=1
    for f in usr/share/sddm/themes/hde/Main.qml usr/share/sddm/themes/hde/theme.conf \
             usr/share/sddm/themes/hde/metadata.desktop usr/share/sddm/themes/hde/components/HdePassword.qml \
             usr/share/sddm/themes/hde/assets/background.png etc/sddm.conf.d/50-hde-theme.conf; do
        [ -e "$ROOT/$f" ] || { ok=0; info "missing after install: $f"; }
    done
    if [ "$rc" = 0 ] && [ "$ok" = 1 ]; then pass "installing into a root (--root) puts the theme and the configuration in place"
    else fail "installing into --root $ROOT (exit $rc): $(echo "$out" | tail -n 3 | tr '\n' '|')"; fi
    if grep -q "^\[Theme\]" "$ROOT/etc/sddm.conf.d/50-hde-theme.conf" &&
       grep -q "^Current=hde" "$ROOT/etc/sddm.conf.d/50-hde-theme.conf"; then
        pass "the configuration makes SDDM use the theme ([Theme] Current=hde)"
    else
        fail "the configuration does not select the theme: $(tr '\n' '|' < "$ROOT/etc/sddm.conf.d/50-hde-theme.conf")"
    fi
    if grep -q "^DefaultSession=hde.desktop" "$ROOT/etc/sddm.conf.d/50-hde-theme.conf"; then
        pass "--default-session preselects HDE (DefaultSession=hde.desktop)"
    else
        fail "--default-session did not write DefaultSession"
    fi
    # the theme of an installed HDE is the theme of this checkout: every file the installer put there is identical to
    # its source (the documentation stays in the checkout, and metadata.desktop may have QtVersion rewritten for the
    # greeter this machine has)
    DEST="$ROOT/usr/share/sddm/themes/hde"
    diffs=""
    for f in $(cd "$DEST" && find . -type f | sed 's|^\./||'); do
        if ! cmp -s "$THEME/$f" "$DEST/$f"; then
            if [ "$f" = "metadata.desktop" ] &&
               [ "$(sed -n 's/^QtVersion=//p' "$THEME/$f")" != "$(sed -n 's/^QtVersion=//p' "$DEST/$f")" ] &&
               [ "$(grep -v '^QtVersion=' "$THEME/$f")" = "$(grep -v '^QtVersion=' "$DEST/$f")" ]; then
                continue      # only the greeter line: the installer wrote it for the greeter installed here
            fi
            diffs="$diffs $f"
        fi
    done
    if [ -z "$diffs" ]; then
        pass "the installed theme is the theme of this checkout, file for file ($(find "$DEST" -type f | wc -l) files)"
    else
        fail "the installed theme differs from the checkout in:$diffs"
    fi
    out=$(sh "$INSTALL" --root "$ROOT" --uninstall 2>&1)
    if [ ! -e "$ROOT/usr/share/sddm/themes/hde" ] && [ ! -e "$ROOT/etc/sddm.conf.d/50-hde-theme.conf" ]; then
        pass "--uninstall removes the theme and the configuration it wrote"
    else
        fail "--uninstall left files behind: $(echo "$out" | tail -n 2 | tr '\n' '|')"
    fi
else
    fail "login/sddm/install.sh is missing"
fi

# ===================== 3. the login screen for real (SDDM's greeter) =====================
if [ "$RENDER" = 0 ]; then
    info "render step skipped (--no-render)"
else
    GREETER=""
    for g in "${HDE_SDDM_GREETER:-}" sddm-greeter-qt6 sddm-greeter sddm-greeter-qt5; do
        [ -n "$g" ] || continue
        if command -v "$g" >/dev/null 2>&1; then GREETER=$g; break; fi
    done
    if [ -z "$GREETER" ]; then
        info "SDDM's greeter is not installed here, so the theme cannot be rendered (the files and the installer were checked)"
        info "  install it with: sudo apt install sddm   /   sudo dnf install sddm   (Fedora's is sddm-greeter-qt6)"
    else
        info "greeter: $GREETER ($("$GREETER" --version 2>/dev/null | head -n1))"
        OWN_XVFB=0
        if [ -n "${HDE_SDDM_DISPLAY:-}" ] && DISPLAY="$DISP" $XT popups >/dev/null 2>&1; then
            : # the caller gave us a display that works (the CI jobs do)
        elif [ -n "${DISPLAY:-}" ] && $XT popups >/dev/null 2>&1; then
            DISP=$DISPLAY
        else
            if ! have Xvfb; then
                info "no usable X display and no Xvfb: skipping the render step"
                DISP=""
            else
                Xvfb "$DISP" -screen 0 1280x800x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &
                XVFB=$!
                OWN_XVFB=1
                export DISPLAY="$DISP"
                for _ in $(seq 1 50); do $XT popups >/dev/null 2>&1 && break; sleep 0.2; done
            fi
        fi
        export DISPLAY="$DISP"
        if [ -n "$DISP" ] && $XT popups >/dev/null 2>&1; then
            # SDDM 0.20 and newer: --test; older: --test-mode
            GOPID=""
            for flag in --test --test-mode; do
                QT_QUICK_BACKEND="${QT_QUICK_BACKEND:-software}" QT_LOGGING_RULES="*.debug=true" \
                    "$GREETER" "$flag" --theme "$THEME" > "$OUT/greeter.log" 2>&1 &
                gpid=$!
                sleep 4
                if kill -0 "$gpid" 2>/dev/null; then GOPID=$gpid; break; fi
                wait "$gpid" 2>/dev/null
            done
            if [ -z "$GOPID" ]; then
                fail "the greeter did not stay up with --test or --test-mode: $(tail -n 3 "$OUT/greeter.log" | tr '\n' '|')"
            else
                pass "the greeter starts and stays up with the theme ($GREETER)"
                # the greeter read *our* theme (not a fallback: SDDM silently falls back when the metadata is wrong)
                if grep -q "Loading theme configuration from $THEME/theme.conf" "$OUT/greeter.log"; then
                    pass "the greeter loaded this theme's configuration"
                else
                    fail "the greeter did not load $THEME/theme.conf: $(grep -i 'theme' "$OUT/greeter.log" | head -n 2 | tr '\n' '|')"
                fi
                # no QML error: this is what a theme that only *looks* fine fails on
                errs=$(grep -nE "failed to load component|Cannot find file|is not a type|ReferenceError|TypeError|SyntaxError|Unable to assign|Unable to determine|Cannot assign|is not a function" "$OUT/greeter.log" | head -n 5)
                if [ -z "$errs" ]; then
                    pass "the QML of the theme runs without an error in the greeter ($(grep -c . "$OUT/greeter.log") log lines)"
                else
                    fail "the QML of the theme reports errors:"; echo "$errs" | sed 's/^/INFO:   /' >> "$OUT/results.txt"
                    echo "$errs" | head -n 3 | sed 's/^/        /'
                fi
                # ... and it drew what it is supposed to draw: a dark screen, the card, the accent line on the card
                W=1280; H=800
                geo=$($XT geometry "$($XT root-window "_NET_SUPPORTING_WM_CHECK" 2>/dev/null)" 2>/dev/null)
                corner=$($XT pixel 6 6 2>/dev/null)
                sum=$(echo "$corner" | awk '{print ($1+$2+$3)}')
                if [ -n "$corner" ] && [ "${sum:-0}" -lt 200 ]; then
                    pass "the background is drawn and dark (top-left pixel $corner)"
                else
                    fail "the top-left pixel is '${corner:-?}': the background is missing or not dark"
                fi
                accent=0; card=0
                y=0
                while [ "$y" -lt "$H" ]; do
                    c=$($XT pixel 640 "$y" 2>/dev/null)
                    if [ -n "$c" ]; then
                        # shellcheck disable=SC2086
                        set -- $c
                        if [ "$1" -ge 25 ] && [ "$1" -le 95 ] && [ "$2" -ge 90 ] && [ "$2" -le 175 ] && [ "$3" -ge 180 ]; then
                            accent=$((accent + 1))
                        fi
                        if [ "$1" -le 40 ] && [ "$2" -le 45 ] && [ "$3" -ge 22 ] && [ "$3" -le 70 ]; then
                            card=$((card + 1))
                        fi
                    fi
                    y=$((y + 4))
                done
                if [ "$accent" -ge 1 ] && [ "$card" -ge 10 ]; then
                    pass "the login card is drawn in the middle of the screen, with the accent colour on it ($card card pixels, $accent accent pixels)"
                else
                    fail "the login card is missing ($card card pixels, $accent accent pixels at x=640)"
                fi
                # a screenshot for the CI report
                if have import; then
                    import -display "$DISPLAY" -window root "$OUT/shot-sddm-login.png" 2>/dev/null &&
                        info "screenshot: $OUT/shot-sddm-login.png"
                fi
                # the greeter must not have crashed while we looked at it
                if kill -0 "$GOPID" 2>/dev/null; then
                    pass "the greeter is still running after the checks"
                else
                    fail "the greeter exited during the checks: $(tail -n 2 "$OUT/greeter.log" | tr '\n' '|')"
                fi
                kill "$GOPID" 2>/dev/null
                sleep 1
                kill -9 "$GOPID" 2>/dev/null
            fi
        else
            info "no X display could be started: skipping the render step"
        fi
        [ "${OWN_XVFB:-0}" = 1 ] && kill "${XVFB:-0}" 2>/dev/null
    fi
fi

echo ""
if [ "$FAILS" = 0 ]; then
    echo "== sddm-test: all checks passed (0 failed)" | tee -a "$OUT/results.txt"
else
    echo "== sddm-test: $FAILS check(s) failed" | tee -a "$OUT/results.txt"
fi
exit "$FAILS"
