#!/bin/sh
# tests/nexwm-watchdog.sh — the leash of tests/nexwm-test.sh: a test that hangs must end with something to read.
#
#   sh tests/nexwm-watchdog.sh <pid of the test> <its output directory> <seconds>
#
# A CI job that hangs tells nobody anything, so this says it once — a FAIL line in results.txt naming the last thing the
# test said, which tests/ci-annotate.py publishes — and then asks the test to end with a TERM. The test answers that
# signal by putting away everything it started (see the traps at the top of the test), but a shell cannot act on a
# signal while it is waiting for a command that never returns: after a grace period the leash therefore kills
# everything the test started (the X server, the window managers, the tool that is not answering) and then the test
# itself, so the step ends and its result can be read.
#
# It is a separate program rather than a subshell of the test for two reasons:
#   * `$$` inside `( … )` is still the pid of the test, and `$1`/`$2` of a `sh -c` body are expanded inside the awk
#     programs below — a watchdog written that way hunts for the wrong pids and kills itself first (it did);
#   * a separate file can be run and tested on its own (tests/nexwm-test.sh only starts it).
#
# Nothing of it goes to the stdout of the step: a leftover of a test that finished must never keep the pipe of a CI step
# open (that alone would hang a job until the runner gives up on it) — the caller redirects it.
#
# Environment: HDE_TEST_WATCHDOG is the number of seconds the test is given (the caller passes it in; 300 by default).
set -u
main=$1                 # the test
out=$2                  # where its results.txt is
secs=${3:-300}

sleep "$secs"
kill -0 "$main" 2>/dev/null || exit 0              # the test is over: nothing to say

printf 'FAIL: nexwm: still running after %ss (the last thing it said: %s)\n' "$secs" \
       "$(tail -n 1 "$out/results.txt" 2>/dev/null | cut -c1-200)" >> "$out/results.txt"

kill -TERM "$main" 2>/dev/null                     # the trap in the test puts its world away and leaves
sleep 20
kill -0 "$main" 2>/dev/null || exit 0              # it did: nothing more to do

printf 'FAIL: nexwm: it did not end on its own (killed, and everything it started with it)\n' >> "$out/results.txt"
for p in $(ps -eo pid=,ppid= 2>/dev/null | awk -v m="$main" -v self="$$" '$2 == m && $1 != self { print $1 }'); do
    kill -KILL "$p" 2>/dev/null                    # everything the test started, including the tool it waits for
done
kill -KILL "$main" 2>/dev/null                     # ... and the test itself, last
