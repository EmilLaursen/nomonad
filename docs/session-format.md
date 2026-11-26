# nomonad session trace format

A line-oriented, text trace emitted by the in-process recorder
(`src/recorder.c`, gated by `NM_RECORD`) and consumed by the replayer
(`test/harness/replay.c`). One event per line. Variable-length
payloads (config text) use an explicit `bytes=N` prefix followed by N
verbatim bytes on the next line.

The format is intended to be:

- **Inspectable.** A human can `tail -f` the file and read what is
  happening. `grep` / `awk` work for slicing.
- **Stream-safe.** Each line is self-contained, so a partially-written
  trace from a crashed session is parseable up to the last complete
  line.
- **Throwaway across schema bumps.** No forward/backward compatibility
  story: bump the version, re-record. The recorder is in-tree.

## Header

The first line of every trace is exactly:

```text
nomonad-trace v=1
```

Parsers should reject anything else.

## Line shape

Every event line after the header has the form:

```text
T <ns> <kind> [<key>=<value>]...
```

- `T` is a literal byte. Cheap visual anchor for `grep -v` and so a
  parser can reject lines that lost their leading byte.
- `<ns>` is the timestamp in nanoseconds since recording started
  (`CLOCK_MONOTONIC`, recorder zeros the clock at `recorder_init`).
- `<kind>` is a lowercase identifier (`window_add`, `press`,
  `output_remove`, ...).
- `<key>=<value>` pairs are unordered; recorders should still emit them
  in the order documented below for readability. Values are unquoted
  decimal integers, unquoted bareword tokens, or double-quoted strings
  (see "Strings" below).

## Strings

Strings are double-quoted. Inside a string the following escapes are
honored: `\\`, `\"`, `\n`, `\t`. Any byte outside printable ASCII
(`0x20..0x7E`) other than the above escapes is encoded as `\xNN` (two
lowercase hex digits). The recorder never emits raw newlines or NULs
inside a quoted string.

`""` represents the empty string. A field whose value is logically
absent uses the bareword `none` (e.g. `parent=none`).

## Long payloads

Two events carry blob-shaped data (the full config text). They use a
length prefix on their event line, then the body verbatim on the next
line, then exactly one trailing `\n`:

```text
T <ns> config_set bytes=215
(defcfg (layout bsp))
...213 more bytes of config...

T <ns> config_reload bytes=...
...
```

The body is opaque to the parser; it is fed verbatim into
`config_load_string` / `harness_stage_reload` at replay time.

## Identifiers

Every output, seat, and window referenced in the trace has a stable
numeric id assigned by the recorder the first time it sees the object.
Ids are monotonic per kind, starting at 1. **Id `0` is reserved and
means "none"** (used for `parent=none`-style cases when the consumer
prefers an integer).

Replay maintains its own `recorded_id -> harness pointer` table; the
recorder's pointers are not exported.

## Event vocabulary

Order below mirrors the typical order in a fresh session.

### Meta

| event           | params                                  | meaning                                                       |
|-----------------|-----------------------------------------|---------------------------------------------------------------|
| `start`         | `unix=<sec>`                            | Always the second line. `unix` is a wall-clock anchor only.   |
| `config_set`    | `bytes=<N>`                             | Initial config text. Body follows on the next line.           |
| `end`           | -                                       | Clean shutdown sentinel. Absence means "trace was truncated". |

### Outputs

| event               | params                                  |
|---------------------|-----------------------------------------|
| `output_add`        | `id=<n> x=<x> y=<y> w=<w> h=<h>`        |
| `output_position`   | `id=<n> x=<x> y=<y>`                    |
| `output_dimensions` | `id=<n> w=<w> h=<h>`                    |
| `output_remove`     | `id=<n>`                                |
| `output_usable`     | `id=<n> x=<x> y=<y> w=<w> h=<h>`        |

`output_add` is emitted on the river `output` event; `_position` /
`_dimensions` only when the geometry changes mid-session. `output_usable`
is the layer-shell non-exclusive area (global coordinates), emitted when
it changes.

### Seats

| event        | params       |
|--------------|--------------|
| `seat_add`   | `id=<n>`     |
| `seat_remove`| `id=<n>`     |

### Windows

| event                     | params                                                                       |
|---------------------------|------------------------------------------------------------------------------|
| `window_add`              | `id=<n>`                                                                     |
| `window_app_id`           | `id=<n> app_id=<string\|none>`                                               |
| `window_title`            | `id=<n> title=<string\|none>` (gated by `NM_RECORD_TITLES`, off by default)  |
| `window_parent`           | `id=<n> parent=<p\|none>`                                                    |
| `window_dimensions_hint`  | `id=<n> min_w=<a> min_h=<b> max_w=<c> max_h=<d>`                             |
| `window_fullscreen`       | `id=<n> on=<0\|1>`                                                           |
| `window_close`            | `id=<n>` (server-side `.closed`; replay calls `harness_close_window`)        |

`window_add` itself carries no app_id or title because river fires those
as separate `app_id` / `title` events on the new window. The replayer
needs the same separation so the synthesized order matches production.

### Pointer / seat events

| event                | params                                | notes                                         |
|----------------------|---------------------------------------|-----------------------------------------------|
| `pointer_to`         | `seat=<s> output=<o>`                 | Emitted when `seat->output` flips.            |
| `pointer_position`   | `seat=<s> x=<x> y=<y>`                | Only if `NM_RECORD_POINTER=full`.             |
| `pointer_enter`      | `seat=<s> window=<w>`                 |                                               |
| `pointer_leave`      | `seat=<s>`                            |                                               |
| `op_delta`           | `seat=<s> dx=<x> dy=<y>`              | Drag delta from river.                        |
| `op_release`         | `seat=<s>`                            |                                               |
| `window_interaction` | `seat=<s> window=<w>`                 | River-side focus signal.                      |
| `ate_unbound`        | `seat=<s>`                            | Submap teardown trigger.                      |

### Bindings

| event           | params                                            | notes                                                   |
|-----------------|---------------------------------------------------|---------------------------------------------------------|
| `press`         | `seat=<s> kind=<KIND> arg=<string\|none>`         | Captured at `xkb_binding_handle_pressed`.               |
| `pointer_press` | `seat=<s> kind=<KIND> arg=<string\|none>`         | Captured at `pointer_binding_handle_pressed`.           |
| `repeat_press`  | `seat=<s> kind=<KIND> arg=<string\|none>`         | Each `drain_repeat_timer` fire that re-arms the action. |
| `stop_repeat`   | `seat=<s>`                                        |                                                         |

`KIND` is the symbolic name returned by `action_kind_name` (e.g.
`FOCUS_NEXT`, `MOVE_TO_WORKSPACE`, `LAYOUT_VERB`). The full enum lives
in `src/nomonad.h`. Replay calls `harness_press(seat, kind, arg)`
directly; the keysym -> binding -> dispatch chain is not replayed.

### Lifecycle

| event              | params       | notes                                                                |
|--------------------|--------------|----------------------------------------------------------------------|
| `config_reload`    | `bytes=<N>`  | Body follows. Replay calls `harness_stage_reload(body)` then dispatches `RELOAD_CONFIG`. |
| `session_locked`   | -            | River sent `wm_listener.session_locked`. Recorded for fidelity; replay is a no-op today. |
| `session_unlocked` | -            | Same.                                                                |

## Privacy gates

All gated by env vars at recorder init. Defaults are the most-private
choice that still produces a useful replay trace.

| var                  | values            | default  | effect                                                        |
|----------------------|-------------------|----------|---------------------------------------------------------------|
| `NM_RECORD_TITLES`   | `0\|1\|hash`      | `0`      | `0` omits `window_title` events; `hash` emits a stable FNV1a64 hex of the title; `1` emits the title verbatim. |
| `NM_RECORD_APPID`    | `1\|hash`         | `1`      | `1` emits app_id verbatim (`defrules` matchers need this); `hash` emits FNV1a64 hex (defrules will not match, but layout-only replay still works). |
| `NM_RECORD_POINTER`  | `output\|full`    | `output` | `output` emits `pointer_to` only on output crossings; `full` emits every `pointer_position`. |

## Throttling

Beyond the pointer gate above, the recorder coalesces nothing. Every
listener event becomes one line; `window.c`'s existing `position_synced`
/ `dimensions_synced` caches already suppress redundant requests at the
WM layer, so the trace inherits that quiet steady state.

## Capacity

| var                  | default    | effect                                                                       |
|----------------------|------------|------------------------------------------------------------------------------|
| `NM_RECORD`          | unset      | Path of trace file. Unset disables recording. `-` is not special; pick a real path. |
| `NM_RECORD_MAX_BYTES`| `67108864` | After this many bytes written, recorder logs to stderr and stops. Replay still works on the prefix. |

The recorder does no in-memory accumulation: the file is opened at init
with stdio line buffering, each event becomes a line, normal kernel
writeback handles persistence. `SIGTERM`-killed sessions lose at most
the in-flight line. The recorder installs `atexit(recorder_shutdown)`
so any normal `exit(3)` path flushes and closes cleanly.

Per-event syscall overhead with line buffering is ~200 ns on Linux,
negligible against the manage cycle's microsecond budget.

## Versioning

`v=1` is the only accepted version. Schema changes bump the version
prefix in the header and force a re-record. There is no migration path
and no compatibility shim. The trace format is in-tree; the recorder
and replayer are always on the same schema as their source tree.

## Example

```text
nomonad-trace v=1
T 0 start unix=1714750000
T 0 config_set bytes=23
(defcfg (layout bsp))
T 110 output_add id=1 x=0 y=0 w=1920 h=1080
T 195 seat_add id=1
T 220000 window_add id=1
T 220150 window_app_id id=1 app_id="org.mozilla.firefox"
T 220180 window_parent id=1 parent=none
T 480000 press seat=1 kind=FOCUS_NEXT arg=none
T 1200500 window_add id=2
T 1200620 window_app_id id=2 app_id="alacritty"
T 4500000 press seat=1 kind=WORKSPACE arg="2"
T 4502000 pointer_to seat=1 output=1
T 9100000 window_close id=2
T 9300000 end
```

The replayer reads this top-down, building harness state from the
output/seat/window events and dispatching each `press` via
`harness_press`. Timestamps drive the inter-event sleep when `--realtime`
is set; under `--asap` they are recorded in stats but not slept on.
