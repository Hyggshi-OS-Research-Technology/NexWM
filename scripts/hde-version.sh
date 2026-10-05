#!/bin/sh
# scripts/hde-version.sh OUT.h — write OUT.h with HDE_VERSION ("<commit> <date>") when it changed (run by `make`).
# The version comes from git in a clone, from data/version in a GitHub ZIP / tarball (git archive fills it in, see
# .gitattributes), else "unknown". Only read-only git commands: `sudo make install` must not write into .git.
cd "$(dirname "$0")/.." || exit 1
out=$1
v=""
if [ -e .git ] && command -v git >/dev/null 2>&1; then
    v=$(git -c safe.directory='*' log -1 --format='%h %cd' --date=short 2>/dev/null)
fi
if [ -z "$v" ] && [ -f data/version ]; then
    # shellcheck disable=SC2016  # a literal $Format: the file was not filled in by git archive
    v=$(sed -n '1{/\$Format/!p;}' data/version)
fi
[ -n "$v" ] || v=unknown
new="#define HDE_VERSION \"$v\""
if [ -f "$out" ]; then
    [ "$(cat "$out")" = "$new" ] && exit 0
    [ "$v" = unknown ] && exit 0        # e.g. git refused under sudo: keep what the normal build wrote
fi
echo "$new" > "$out"
