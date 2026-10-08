#!/bin/sh
# tests/arch-test.sh — HDE on Arch Linux, the parts that do not need X: this really is an Arch, the dependency list of
# the repository (packaging/arch/deps.sh), and HDE speaking Arch's package manager (pacman, and Arch's package names).
#
#   sh tests/arch-test.sh            everything (needs pacman: it is meant for an Arch machine or container)
#   sh tests/arch-test.sh --deps     the repository's Arch list only: no pacman, no Arch needed
#
# The CI job "Arch Linux (pacman, current wlroots)" runs this first (--deps), then the whole smoke test
# (tests/smoke.sh) in Xvfb — which is also where the pictures of the Arch desktop come from.
#
# Environment: HDE_TEST_OUT (/tmp/hde-arch), BUILD (build). Exit status = failures; results.txt has the PASS/FAIL lines.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CASE=$(cd "$HERE/.." && pwd)
BUILD=${BUILD:-$CASE/build}
OUT=${HDE_TEST_OUT:-/tmp/hde-arch}
DEPS="$CASE/packaging/arch/deps.sh"
MODE=${1:-all}

mkdir -p "$OUT"
FAILS=0
pass() { echo "PASS: arch: $*" | tee -a "$OUT/results.txt"; }
fail() { echo "FAIL: arch: $*" | tee -a "$OUT/results.txt"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: arch: $*" | tee -a "$OUT/results.txt"; }
skip() { echo "SKIP: arch: $*" | tee -a "$OUT/results.txt"; }
check() { d=$1; shift; if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---------- 1. the repository's Arch list ----------
if [ -x "$DEPS" ] || [ -f "$DEPS" ]; then
    build=$(sh "$DEPS" list build 2>/dev/null)
    n=$(printf '%s\n' "$build" | grep -c .)
    if [ "$n" -ge 15 ]; then pass "packaging/arch/deps.sh lists the build dependencies ($n entries)"
    else fail "packaging/arch/deps.sh lists the build dependencies ($n entries, expected at least 15)"; fi
    # the Arch names are the library names: no -dev, no -devel (that is what Arch users would type and pacman knows)
    # no Debian or Fedora name may survive (the names of the other two lists, where this one has the library itself).
    # `base-devel` and `wayland-protocols` are Arch's own names and end in -devel/-tools without being one of those.
    bad=""
    for name in libgtk-3-dev gtk3-devel libwnck-3-dev libwnck3-devel libxcb1-dev libxcb-devel libpam0g-dev pam-devel \
                libwlroots-dev wlroots-devel libgtk-layer-shell-dev gtk-layer-shell-devel pkgconf-pkg-config pkg-config \
                build-essential xorg-x11-server-utils xorg-x11-utils libnotify-bin network-manager; do
        printf '%s\n' "$build" | grep -qx "$name" && bad="$bad $name"
    done
    if [ -z "$bad" ]; then pass "the Arch build list has Arch's names, no Debian/Fedora ones (gtk3, libwnck3, libxcb, pam, wlroots)"
    else fail "the Arch build list has a Debian/Fedora name:$bad"; fi
    # the three that carry a feature of HDE's own and are easy to leave out
    for p in libxcb pam wlroots; do
        if printf '%s\n' "$build" | grep -qx "$p"; then pass "the build list has $p (NexWM's X11 side / hde-lock / the Wayland compositor)"
        else fail "the build list is missing $p"; fi
    done
    for g in runtime-minimal runtime-full test wayland-session; do
        c=$(sh "$DEPS" list "$g" 2>/dev/null | grep -c .)
        if [ "$c" -ge 3 ]; then pass "deps.sh lists $g ($c entries)"; else fail "deps.sh list $g ($c entries)"; fi
    done
    check "deps.sh says which groups it has" sh "$DEPS" groups
    if sh "$DEPS" list nonsense 2>&1 | grep -q "unknown group"; then pass "an unknown group is said, not ignored"
    else fail "an unknown group is ignored"; fi
else
    fail "packaging/arch/deps.sh is not there"
fi

[ "$MODE" = --deps ] && {
    if [ "$FAILS" = 0 ]; then echo "== arch-test (--deps): all checks passed" | tee -a "$OUT/results.txt"
    else echo "== arch-test (--deps): $FAILS check(s) FAILED" | tee -a "$OUT/results.txt"; fi
    exit "$FAILS"
}

# ---------- 2. this really is an Arch (the rest of the test would prove nothing on another system) ----------
# `make check-unit` runs this on every distribution: everywhere else, what is checked is the list in the repository
# (packaging/arch/deps.sh), which is a check that can be made anywhere, and the machine-specific half is skipped.
if ! grep -qE '^(ID=arch|ID_LIKE=.*arch)' /etc/os-release 2>/dev/null && ! have pacman; then
    skip "this is not an Arch ($(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"')): only the Arch list was checked"
    echo ""
    if [ "$FAILS" = 0 ]; then
        echo "== arch-test: the Arch list passed, the rest was skipped (not an Arch)" | tee -a "$OUT/results.txt"
        exit 0
    fi
    echo "== arch-test: $FAILS check(s) failed" | tee -a "$OUT/results.txt"
    exit "$FAILS"
fi
if grep -qE '^(ID=arch|ID_LIKE=.*arch)' /etc/os-release 2>/dev/null; then
    pass "Arch: $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | tr -d '"')"
else
    fail "Arch: this is not an Arch (/etc/os-release: $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"'))"
fi
if have pacman; then pass "pacman $(pacman -V 2>/dev/null | head -n1 | tr -s ' ' | cut -d' ' -f3-) is here"
else fail "pacman is not here: this script is for Arch Linux"; fi

# ---------- 3. the dependency list against this machine ----------
if have pacman; then
    miss=$(sh "$DEPS" missing build 2>/dev/null | grep -v '^note:' | tr '\n' ' ')
    if [ -z "$miss" ]; then pass "every build dependency of packaging/arch/deps.sh is installed"
    else fail "build dependencies missing: $miss"; fi
fi

# ---------- 4. HDE speaks Arch: pacman, and Arch's package names ----------
if [ -x "$BUILD/hde-settings" ]; then
    "$BUILD/hde-settings" --deps build > "$OUT/deps-build.txt" 2>&1
    if grep -q "^  sudo pacman -S " "$OUT/deps-build.txt" && grep -q "gtk3" "$OUT/deps-build.txt" &&
       grep -q "libwnck3" "$OUT/deps-build.txt" && grep -q "libxcb" "$OUT/deps-build.txt" &&
       ! grep -qE "(^| )(libgtk-3-dev|libwnck-3-dev|gtk3-devel|libwnck3-devel|libxcb1-dev|libxcb-devel)( |$)" "$OUT/deps-build.txt"; then
        pass "hde-settings --deps build: $(sed -n 's/^  //p' "$OUT/deps-build.txt" | tr -d '\n' | cut -c1-60)…"
    else
        fail "hde-settings --deps build prints 'sudo pacman -S' with Arch's package names: $(tr '\n' '|' < "$OUT/deps-build.txt")"
    fi
    "$BUILD/hde-settings" --deps runtime > "$OUT/deps-runtime.txt" 2>&1
    if grep -q "^  sudo pacman -S " "$OUT/deps-runtime.txt" && grep -q "networkmanager" "$OUT/deps-runtime.txt"; then
        pass "hde-settings --deps runtime: Debian's network-manager is Arch's networkmanager"
    else
        fail "hde-settings --deps runtime prints pacman with Arch's names: $(tr '\n' '|' < "$OUT/deps-runtime.txt")"
    fi
    sed 's/^/INFO:   /' "$OUT/deps-build.txt" "$OUT/deps-runtime.txt" >> "$OUT/results.txt"
    # a package HDE asks for by its Debian name has to come out as the Arch one (`pacman -S pam`, not libpam0g-dev)
    if "$BUILD/hde-settings" --deps build | grep -q "libpam0g-dev"; then
        fail "a Debian package name survived on Arch (libpam0g-dev)"
    else
        pass "the Debian names of the instruction are translated to Arch's (pam, not libpam0g-dev)"
    fi
else
    skip "hde-settings is not built ($BUILD: run make first)"
fi

# ---------- 5. the Arch half of distro-test (it takes os-release files of its own, so it needs no Arch) ----------
for t in distro-test nexwm-test; do
    if [ -x "$BUILD/$t" ]; then
        if "$BUILD/$t" > "$OUT/$t.txt" 2>&1; then pass "unit test $t: $(grep -c '^PASS' "$OUT/$t.txt") checks passed"
        else fail "unit test $t: $(grep '^FAIL' "$OUT/$t.txt" | head -n 2 | tr '\n' ' ')…"; fi
    else
        skip "unit test $t is not built (make check-unit)"
    fi
done
if [ -x "$BUILD/distro-test" ]; then
    if grep -q "Arch: 'sudo pacman -S gtk3'" "$OUT/distro-test.txt"; then
        pass "distro-test: on Arch the install hint is 'sudo pacman -S' with Arch's package names"
    else
        fail "distro-test: the Arch part did not run ($(grep -c . "$OUT/distro-test.txt" 2>/dev/null | tr -d ' ') lines)"
    fi
fi

# ---------- 6. wlroots: which release this Arch carries, and whether NexWM was built for it ----------
if have pacman && pacman -Qq wlroots >/dev/null 2>&1; then
    v=$(pacman -Q wlroots | awk '{print $2}')
    info "wlroots $v (Arch is rolling: this is the newest release)"
    if [ -x "$BUILD/nexwm" ] && "$BUILD/nexwm" --version 2>&1 | grep -qi "wayland\|wlroots"; then
        pass "NexWM says it has its wlroots compositor ($("$BUILD/nexwm" --version 2>&1 | head -n1))"
    elif [ -x "$BUILD/nexwm" ]; then
        fail "NexWM was built without its wlroots compositor on Arch (wlroots $v is installed: wayland-scanner and wayland-protocols?)"
    else
        skip "nexwm is not built ($BUILD)"
    fi
else
    skip "wlroots is not installed (packaging/arch/deps.sh install build)"
fi

echo ""
if [ "$FAILS" = 0 ]; then
    echo "== arch-test: all checks passed (0 failed)" | tee -a "$OUT/results.txt"
else
    echo "== arch-test: $FAILS check(s) failed" | tee -a "$OUT/results.txt"
fi
exit "$FAILS"
