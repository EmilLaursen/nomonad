#!/usr/bin/env bash
# Launch nomonad inside a nested river compositor on X11 for development.
# Rebuilds nomonad, writes a throwaway river init that execs it, and starts
# river under the wlroots X11 backend so it appears as an X11 window on the
# host session.
#
# Also seeds $XDG_CONFIG_HOME with examples/config.scm so the nested
# instance exercises the config-load path (parsing, defrules, defcfg gaps,
# hot reload via inotify) instead of just config_default(). The host
# session owns the Super chords, so the copy moves them to Alt:
# M -> A, M-S -> A-S, M-A -> A-C, M-A-S -> A-C-S, M-C -> C-S. Edit
# $XDG_CONFIG_HOME/nomonad/config.scm in another terminal to test reload,
# or rerun this script to start fresh.
#
# Usage: ./run-nested.sh [--asan] [extra-river-args...]
#   --asan  rebuild with `make asan` and export ASan/UBSan options so
#           any memory or UB errors abort the nested compositor with a
#           stack trace on stderr.
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
TESTDIR=/tmp/nm-test
USE_ASAN=0

if [ "${1:-}" = "--asan" ]; then
	USE_ASAN=1
	shift
fi

command -v river >/dev/null 2>&1 || {
	echo "error: river not found in PATH" >&2
	exit 1
}

BUILD="$REPO/build"
if [ "$USE_ASAN" = 1 ]; then
	BUILD="$REPO/build-asan"
	make -C "$REPO" asan
	# detect_leaks is on by default on Linux; spell it out for clarity.
	# halt_on_error is what makes UBSan abort instead of just printing.
	export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1"
	export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
else
	make -C "$REPO"
fi

mkdir -p "$TESTDIR/river" "$TESTDIR/nomonad"
cat >"$TESTDIR/river/init" <<EOF
#!/bin/sh
exec "$BUILD/nomonad"
EOF
chmod +x "$TESTDIR/river/init"

sed -e 's/(M-A-S /(A-C-S /g' -e 's/(M-A /(A-C /g' -e 's/(M-C /(C-S /g' \
	-e 's/(M-S /(A-S /g' -e 's/(M /(A /g' \
	"$REPO/examples/config.scm" >"$TESTDIR/nomonad/config.scm"

exec env XDG_CONFIG_HOME="$TESTDIR" WLR_BACKENDS=x11 river "$@"
