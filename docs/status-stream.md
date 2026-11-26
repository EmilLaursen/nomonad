# nomonad status stream

nomonad publishes a logfmt snapshot of WM state on a unix-domain
socket whenever publishable state changes. Designed for waybar
custom modules, libnotify hooks, and any consumer that wants to
mirror "which workspace is window with PID X on" without polling.

Socket path:

```text
$XDG_RUNTIME_DIR/nomonad-status.sock
```

Falls back to `/tmp/nomonad-status-$UID.sock` when XDG_RUNTIME_DIR
is unset. nomonad logs `status broadcast on <path>` on startup.

Subscribe with anything that speaks AF_UNIX SOCK_STREAM. Quick
look:

```sh
socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/nomonad-status.sock
```

Each frame is a sequence of newline-delimited records bracketed by
`type=frame_start seq=<n> time_start_unix_ns=<u64>` and
`type=frame_end seq=<n> time_end_unix_ns=<u64>`. Example:

```text
type=frame_start seq=42 time_start_unix_ns=1746331200123456789
type=output index=0 visible_workspace=2
type=workspace index=2 focused_window=window/foo layout=tall
type=window id=window/foo workspace=2 app_id=foot focused=1 floating=0 fullscreen=0
type=frame_end seq=42 time_end_unix_ns=1746331200123567890
```

The two timestamps come from `CLOCK_REALTIME` (~50 ns total per
cycle via vDSO) sampled at the top of `wm_handle_manage_start` and
right before broadcast. They bracket the WM-side cycle the
snapshot describes, so subscribers can compute cycle duration
(`end - start`) and correlate against journald / syslog without
running their own clock. Timestamps live on the markers, not in
the body, so dedupe is unaffected (the body hash never includes
the time).

Values containing space, `=`, `"` or `\` are wrapped in double
quotes with `"` and `\` backslash-escaped. Integers and short
identifiers are bare. The schema is intentionally narrow; new
fields can be added without breaking existing consumers (parse
key=value pairs, ignore unknown keys).

The publish path is gated by an FNV-1a hash dedupe: rebuilding a
snapshot identical to the last broadcast does not re-send. New
subscribers immediately receive the most recent broadcast as a
replay so a status bar restart paints the right state without
waiting for the next change. Up to 16 simultaneous subscribers;
the 17th gets `type=error reason=too_many_subscribers` and a
close. Subscribers that fail to drain on EAGAIN are dropped.

Privacy gates (consulted at startup):

- `NM_STATUS_TITLES` on/off, default off. Titles can leak URLs,
  file paths, recently-typed search queries.
- `NM_STATUS_APPID` on/off, default on. The wayland class
  equivalent; rarely sensitive.
- `NM_STATUS_PID` on/off, default off. Exposes the
  `unreliable_pid` event surface (river protocol v2+).
- `NM_STATUS_DEBUG` on/off, default off. Stderr line per publish
  with byte count, subscriber count, and seq.

Example shell consumer that prints "window <app_id> on workspace
<n>" each time the focus changes:

```sh
socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/nomonad-status.sock |
    awk -F'[ =]' '
        /^type=window/ {
            fields["app_id"] = ""
            for (i = 2; i < NF; i += 2) fields[$i] = $(i+1)
            if (fields["focused"] == "1")
                print "focus:", fields["app_id"], "ws", fields["workspace"]
        }'
```

If the socket cannot be bound (path too long, runtime dir
unwritable, ...) nomonad logs the underlying errno to stderr and
keeps running with no status broadcast. The WM cycle is unaffected.
