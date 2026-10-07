#!/bin/sh
# tests/choose-run-test.sh — hde-choose, the program of "which program for this?", driven for real.
#
# The rules (the table, what is installed, what was remembered) are checked without a display by tests/choose-test.c.
# This one runs the program itself, with programs on a PATH of its own and a settings file of its own, and looks at what
# it started (stdout) and what it said (stderr):
#
#   1. the help, the version, --list, and the mistakes on the command line (exit status 2);
#   2. what it starts: one program installed (no question — and the log says why), several installed with no display
#      (the first one of the table, and the log says there was nothing to ask on), the arguments of the caller are
#      passed on, and the arguments a program needs itself are used only when the caller passed none
#      (hde-choose files -> the home folder, hde-choose files /tmp -> /tmp);
#   3. what it remembers: --set writes choice_<feature> into settings.ini and then it is used without asking; a choice
#      that is not installed any more falls back and says so; "ask" is an answer like any other; HDE_CHOOSE=PROGRAM
#      overrides everything for one run; --reset forgets an answer and leaves the rest of settings.ini alone;
#   4. nothing installed at all: exit status 127, and a line naming a program to install on this system;
#   5. the question itself, through the stand-in for GTK3 of this test (tests/choose-stub/): the line that was chosen
#      is the one that starts, and "Remember my choice" decides whether it is written down — including leaving the
#      question without choosing anything (nothing starts, exit status 1). The stand-in says what the user "clicked"
#      through HDE_CHOOSE_STUB, the way the stand-in for mpv says what the player is doing.
#
#   sh tests/choose-run-test.sh
#
# Needs: the program (the Makefile builds build/hde-choose; it is a GTK program, so without libgtk-3-dev there is none
# and this test says SKIP). Nothing of the user is touched: the PATH, HOME and XDG_CONFIG_HOME of the program point
# into a directory of this test's own. Exit status = failures.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-build}
B=$(cd "$BUILD" 2>/dev/null && pwd) || { echo "build dir '$BUILD' not found — run make first" >&2; exit 2; }
OUT=${HDE_TEST_OUT:-/tmp/hde-choose-run}
BIN="$B/hde-choose"

FAILS=0
pass() { echo "PASS: choose-run: $*"; }
fail() { echo "FAIL: choose-run: $*"; FAILS=$((FAILS + 1)); }
info() { echo "INFO: choose-run: $*"; }
eq() { if [ "$2" = "$3" ]; then pass "$1: $3"; else fail "$1: expected '$2', got '$3'"; fi; }
has() { case "$3" in *"$2"*) pass "$1" ;; *) fail "$1 ('$2' is not in '$(printf '%s' "$3" | tr '\n' '|')')" ;; esac; }
hasnt() { case "$3" in *"$2"*) fail "$1 ('$2' is in '$(printf '%s' "$3" | tr '\n' '|')')" ;; *) pass "$1" ;; esac; }

# The real program when it is built (a CI machine builds it and has no display, which is one of the things checked
# here), the stand-in for GTK3 otherwise (tests/choose-stub/; its default is "no display" too, so these checks mean the
# same thing on a machine without libgtk-3-dev).
STUB="$B/hde-choose-stub"
if [ ! -x "$BIN" ]; then
    if [ -x "$STUB" ]; then
        info "build/hde-choose is not built (libgtk-3-dev?): the stand-in for GTK3 answers instead"
        BIN="$STUB"
    else
        info "$BIN is not built (libgtk-3-dev is what builds it) — hde-choose is not tested here"
        echo "SKIP: choose-run: build/hde-choose is not built"
        exit 0
    fi
fi

rm -rf "$OUT"
mkdir -p "$OUT/bin/one" "$OUT/empty" "$OUT/home" "$OUT/config"
SETTINGS="$OUT/config/hde/settings.ini"

# a small machine of our own: each program says its name and its arguments, so what hde-choose started is read right
# off stdout. They are shell scripts, so /bin/sh runs them wherever the PATH of the program points.
for p in xfce4-terminal xterm thunar hde-files hde-media mpv; do
    printf '#!/bin/sh\necho %s "$@"\n' "$p" > "$OUT/bin/$p"
    chmod +x "$OUT/bin/$p"
done
cp "$OUT/bin/xterm" "$OUT/bin/one/xterm"     # a machine with exactly one program for the terminal

# what the program sees: a PATH, a HOME and a settings.ini of its own. The script's own commands keep the PATH of the
# machine that runs this test — only the program gets the one below (env sets them for that one command).
run() { _path=$1; shift; env PATH="$_path" HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/config" "$BIN" "$@"; }
run_def() { run "$OUT/bin" "$@"; }
# the program's stdout (the program it started) and stderr (what it said) apart: this test looks at both
start() { stdout=$(run_def "$@" 2>"$OUT/err"); rc=$?; stderr=$(cat "$OUT/err"); }

# ---- 1. the command line -------------------------------------------------------------------------------------
out=$(env XDG_CONFIG_HOME="$OUT/config" "$BIN" --version 2>&1); rc=$?
eq "--version works" 0 "$rc"
has "--version says what this is" "hde-choose (HDE)" "$out"
out=$(env XDG_CONFIG_HOME="$OUT/config" "$BIN" --help 2>&1); rc=$?
eq "--help works" 0 "$rc"
has "--help lists the features" "terminal" "$out"
out=$(env XDG_CONFIG_HOME="$OUT/config" "$BIN" --nonsense 2>&1); rc=$?
eq "an option nobody knows is a mistake (exit status 2)" 2 "$rc"
has "... and says which option it was" "unknown option '--nonsense'" "$out"
out=$(env XDG_CONFIG_HOME="$OUT/config" "$BIN" nonsense 2>&1); rc=$?
eq "a feature nobody knows is a mistake (exit status 2)" 2 "$rc"
has "... and says that it is not a feature" "is not a feature" "$out"
out=$(env XDG_CONFIG_HOME="$OUT/config" "$BIN" 2>&1); rc=$?
eq "no feature at all is a mistake (exit status 2)" 2 "$rc"

# ---- 2. what it starts ---------------------------------------------------------------------------------------
stdout=$(run "$OUT/empty" terminal 2>"$OUT/err"); rc=$?
eq "nothing installed: exit status 127" 127 "$rc"
has "... says that nothing on this machine can do it" "nothing on this machine can do this" "$(cat "$OUT/err")"
has "... and what this system would install" "xterm" "$(cat "$OUT/err")"

start_with_one() { stdout=$(run "$OUT/bin/one" "$@" 2>"$OUT/err"); rc=$?; stderr=$(cat "$OUT/err"); }
start_with_one terminal
eq "one program installed: it is started, and nothing is asked" "xterm" "$stdout"
has "... the log says why not" "only one program" "$stderr"

start terminal
eq "several installed and no display: it starts without asking" "xfce4-terminal" "$stdout"
has "... and the log says there was nothing to ask on" "no display" "$stderr"
eq "... the first one of the table is the one that starts" "xfce4-terminal" "$stdout"

start --no-ask terminal
eq "--no-ask starts the first one too" "xfce4-terminal" "$stdout"
has "... and says it was asked not to ask" "--no-ask" "$stderr"

start terminal -e top
eq "the arguments of the caller are passed on" "xfce4-terminal -e top" "$stdout"
start files
eq "hde-choose files opens the home folder (the argument from the table)" "hde-files $OUT/home" "$stdout"
start files /tmp
eq "... and a place the caller gives wins over it" "hde-files /tmp" "$stdout"
start files
eq "HDE's own file manager comes before the others" "hde-files $OUT/home" "$stdout"

# ---- 3. what it remembers ------------------------------------------------------------------------------------
env XDG_CONFIG_HOME="$OUT/config" "$BIN" --set terminal xterm >/dev/null 2>&1; rc=$?
eq "--set works" 0 "$rc"
has "... and writes it into settings.ini" "choice_terminal=xterm" "$(cat "$SETTINGS" 2>/dev/null)"
out=$(run_def --list 2>/dev/null)
has "--list shows the answer of the user" "remembered: xterm" "$out"
has "--list shows what would run" "will run:   xterm" "$out"

start terminal
eq "a remembered program starts without asking" "xterm" "$stdout"
has "... and the log says it was remembered" "remembered -> xterm" "$stderr"

env XDG_CONFIG_HOME="$OUT/config" "$BIN" --set terminal konsole >/dev/null 2>&1
start terminal
has "a remembered program that is gone is reported" "'konsole' was remembered but is not installed any more" "$stderr"
eq "... and the first installed one is used instead" "xfce4-terminal" "$stdout"

env XDG_CONFIG_HOME="$OUT/config" "$BIN" --set terminal ask >/dev/null 2>&1
has "\"ask\" is an answer like any other" "choice_terminal=ask" "$(cat "$SETTINGS")"
start terminal
has "... and then no remembered program is used" "no display" "$stderr"

stdout=$(env HDE_CHOOSE=xterm PATH="$OUT/bin" HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/config" "$BIN" terminal 2>"$OUT/err")
eq "HDE_CHOOSE overrides even a remembered program" "xterm" "$stdout"
has "... and the log says where the answer came from" "HDE_CHOOSE says xterm" "$(cat "$OUT/err")"
env HDE_CHOOSE=/no/such/program XDG_CONFIG_HOME="$OUT/config" "$BIN" terminal >/dev/null 2>&1; rc=$?
eq "HDE_CHOOSE with a program that is not there: exit status 127" 127 "$rc"

env XDG_CONFIG_HOME="$OUT/config" "$BIN" --set terminal xterm >/dev/null 2>&1
printf 'wm=nexwm\nchoice_files=hde-files\n' >> "$SETTINGS"
env XDG_CONFIG_HOME="$OUT/config" "$BIN" --reset terminal >/dev/null 2>&1; rc=$?
eq "--reset works" 0 "$rc"
hasnt "... the answer is gone" "choice_terminal=" "$(cat "$SETTINGS")"
has "... and the rest of settings.ini is untouched" "wm=nexwm" "$(cat "$SETTINGS")"
has "... including the answer of another feature" "choice_files=hde-files" "$(cat "$SETTINGS")"

# ---- 5. the question itself, through the stand-in for GTK3 ------------------------------------------------
# A question in a test needs a user, and CI has none: tests/choose-stub/ is a stand-in for the toolkit that answers it
# the way the test tells it to (HDE_CHOOSE_STUB=choice2: the second line was clicked; remember0: "Remember my choice"
# was left out; cancel: the question was closed). What it checks is exactly what a user would see: the line that was
# clicked is the one that starts, the programs that can do it are all offered, and only "Remember my choice" writes
# the answer down.
if [ -x "$STUB" ]; then
    # what the user "clicked" goes to the program itself: env, not a shell variable, so that it arrives whatever this
    # script is run with (see tests/choose-stub/gtk.c)
    q() { _stub=$1; shift
          stdout=$(env HDE_CHOOSE_STUB="$_stub" PATH="$OUT/bin" HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/config" \
                  "$STUB" "$@" 2>"$OUT/err"); rc=$?
          stderr=$(cat "$OUT/err"); }
    rm -f "$SETTINGS"; q choice2 terminal
    eq "the line that was chosen is the one that runs (the 2nd)" "xterm" "$stdout"
    has "... and it was remembered" "choice_terminal=xterm" "$(cat "$SETTINGS" 2>/dev/null)"
    has "... the question named the feature" "Which program for a terminal?" "$stderr"
    rm -f "$SETTINGS"; q choice1 terminal
    eq "the first line can be chosen as well" "xfce4-terminal" "$stdout"
    has "... every program that can do it was offered" "radio 2: xterm" "$stderr"
    q choice1 files
    has "... and HDE's own program is marked as HDE's" "hde-files (HDE's own)" "$stderr"

    rm -f "$SETTINGS"; q remember0 terminal
    if [ -s "$SETTINGS" ]; then fail "without \"Remember my choice\" nothing is written down ($(cat "$SETTINGS"))";
    else pass "without \"Remember my choice\" nothing is written down"; fi

    rm -f "$SETTINGS"; q cancel terminal
    eq "closing the question starts nothing (exit status 1)" 1 "$rc"
    has "... and says so" "nothing started" "$stderr"
    if [ -s "$SETTINGS" ]; then fail "and writes nothing down"; else pass "and writes nothing down"; fi
    rm -f "$SETTINGS"
else
    info "build/hde-choose-stub is not built (make build/hde-choose-stub): the question itself was not tested"
fi

echo
if [ "$FAILS" -eq 0 ]; then echo "choose-run-test: all checks passed"; else echo "choose-run-test: $FAILS failed"; fi
exit "$FAILS"
