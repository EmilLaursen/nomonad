#!/bin/sh
name=$1
shift
if [ -S "${XDG_RUNTIME_DIR:-/nonexistent}/systemd/private" ] &&
	command -v systemd-run >/dev/null &&
	! systemctl --user --quiet is-active "$name.scope"; then
	exec systemd-run --user --scope --collect --quiet --slice=session.slice \
		--unit="$name" -- "$@"
fi
exec "$@"
