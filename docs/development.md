# nomonad development

## Build directories

Everything a build produces goes under one output directory, `build/`
by default, the binary at `build/nomonad`. `O=` picks another one, as
in the Linux kernel:

```sh
make O=/tmp/nomonad-build
make test O=/tmp/nomonad-build
```

The sanitizer, profiling and fuzzing builds each get their own
directory unless `O=` is given, so switching between them never
rebuilds the others:

```sh
make asan      # build-asan/: AddressSanitizer + UndefinedBehaviorSanitizer,
               #   -O1 -g -fsanitize=address,undefined
make perf      # build-perf/: frame pointers + debug info, keeps -O2,
               #   -ggdb3 -fno-omit-frame-pointer
make fuzz      # build-fuzz/: the libFuzzer driver, with clang
```

Each directory records its compiler, flags and libraries in `.flags`.
Every object depends on that file, and it is only rewritten when they
change, for example with a different `EXTRA_CFLAGS`. `make clean`
removes the current output directory, `make distclean` removes
`build/`, every `build-*/` and the one `O=` names.

## Nested river

```sh
./run-nested.sh           # nested river under WLR_BACKENDS=x11
./run-nested.sh --asan    # same, with sanitizers + leak reporting
```

The script seeds `/tmp/nm-test/` with a throwaway river init and a copy
of `examples/config.scm` with its Super chords moved to Alt, since the
host session owns Super, then starts river in an X11 window so the
host session is unaffected. With `--asan`, ASAN_OPTIONS and
UBSAN_OPTIONS are exported so any error aborts the nested compositor
with a stack trace on the parent terminal's stderr.

## Headless harness

```sh
make harness              # builds and runs build/test/harness/harness
```

Drives the manage / render cycle in process, with no wayland connection.
`test/harness/fakewl.c` link-wraps the libwayland-client proxy entry
points so the generated `river_*_v1_*` request inlines become no-ops on
synthetic proxies; `test/harness/harness_lib.c` provides main.c's
globals plus the injection / cycle / invariant API
(`harness_add_output`, `harness_add_seat`, `harness_add_window`,
`harness_press`, `harness_cycle`, `harness_teardown`, ...). The
scenarios in `test/harness/harness.c` call those and assert state
invariants on `wm`, `seat->focused`, the workspace lists, etc.

Two runner modes:

```sh
build/test/harness/harness           # forked, one child per scenario
build/test/harness/harness --inproc  # sequential in one process
```

`--inproc` is the mode required by ASan's at-exit leak check (and by
libFuzzer's persistent loop): scenarios run sequentially with
`harness_teardown` between each, so allocations from earlier scenarios
that leaked would be reported on exit. The forked mode trades that for
assertion isolation - one failing scenario does not abort the rest.

## Live per-cycle timing

```sh
NM_TIMING=$HOME/.local/state/nomonad/timing.log ./run-nested.sh
```

Writes one line per manage / render cycle of the running WM:

```text
T <ns> manage_us=<m> render_us=<r> windows=<w> dirty=<0|1>
```

`<ns>` is monotonic nanoseconds since `nomonad` started; the other
fields are wall-clock microseconds spent in `wm_handle_manage_start`
and `wm_handle_render_start` plus the live window count. `dirty=1`
means the cycle observed a publishable state change (focus moved,
window appeared, layout flipped, ...); `dirty=0` is a cycle where
the apply_layout / status broadcast gates would have skipped the
work. Recorder and timer can be on at the same time and capture
independent slices of the same session.

When unset, every timing hook is a single `g_timing == NULL` load +
branch. When set, the recorder `mkdir -p`s the parent dir and prints
`nomonad: writing timing to ...` to stderr; on path failure it logs
`NM_TIMING='...': <errno>; timing disabled` and continues without
timing.

Percentile one-liner:

```sh
sed -n 's/.*manage_us=\([0-9]*\).*/\1/p' timing.log | sort -n | \
    awk '{ v[NR]=$1 } END {
        printf "manage_us  n=%d p50=%d p95=%d p99=%d max=%d\n",
            NR, v[int(NR*0.5)], v[int(NR*0.95)], v[int(NR*0.99)], v[NR]
    }'
```

Swap `manage_us` for `render_us` for the render-half stats.

## Session recorder + replayer

```sh
NM_RECORD=$HOME/.local/state/nomonad/session.log ./run-nested.sh
```

Records the WM event stream to a text trace; see
`docs/session-format.md` for the format. The recorder `mkdir -p`s the
parent directory and prints `nomonad: recording session trace to ...`
to stderr on success. If the path is unwritable it logs
`NM_RECORD='...': <errno>; recording disabled` and continues without
recording. Privacy gates `NM_RECORD_TITLES` (default off),
`NM_RECORD_APPID` (default verbatim), `NM_RECORD_POINTER` (default
output-flips only) tune what gets written. Disabled when `NM_RECORD`
is unset; zero overhead on the manage cycle.

Trace and timing files are created mode 0600. On start the previous
file is kept as `<path>.1`, so restarting after a crash does not
overwrite the trace of the crash. The timing log stops at 64 MiB.

```sh
make replay                                            # builds build/test/harness/replay
build/test/harness/replay session.log                  # run as fast as possible
build/test/harness/replay --realtime session.log
build/test/harness/replay --no-invariants session.log  # perf measurement
```

The replayer drives the headless harness through the recorded events
and prints per-event-kind cycle latency stats at exit, in the same
shape as `--bench-session`'s per-phase summary. `--realtime` sleeps
on the recorded T-deltas so replay tracks the wall-clock cadence of
the original session; default is fastest possible for CI / regression
runs.

## libFuzzer driver

```sh
make fuzz                                   # builds build-fuzz/test/harness/fuzz
```

Builds `build-fuzz/test/harness/fuzz`, a libFuzzer entry point
(`LLVMFuzzerTestOneInput`) that interprets each input byte as an
action selector over a 34-op alphabet covering essentially every
harness-safe action: window add / close (server- and client-
initiated), focus next / prev / master, swap, workspace switch
(numbered, next, prev), move-to-workspace, toggle float /
fullscreen, the four TALL master verbs, the BSP verb table,
reload-config (no-op against an empty staging slot), output add /
remove from a 2x2 grid of monitor layouts, pointer migration
between outputs, the output-targeted focus / move-to actions, and
the full pointer-drag path (pointer_enter / pointer_leave / start
move or resize / op_delta / op_release / `ACTION_MOVE` /
`ACTION_RESIZE`), dynamic scratchpad toggles, and the layer-shell
usable-area and focus events. Each invocation runs `harness_init`, walks the
input one cycle per byte, and `harness_teardown`s.
`harness_check_all` runs inside `harness_cycle` after every step,
so any structural / layout invariant violation aborts and libFuzzer
reports the offending input.

Composed with AddressSanitizer + UndefinedBehaviorSanitizer at -O1 +
frame pointers + debug info; clang-only because libFuzzer is a clang
runtime.

Single-process run (one core, growing corpus on disk):

```sh
mkdir -p test/harness/corpus
build-fuzz/test/harness/fuzz -max_total_time=60 test/harness/corpus/
```

Multi-process run (parallel children sharing the corpus, recommended
for any session longer than a minute):

```sh
build-fuzz/test/harness/fuzz -fork=$(nproc) -max_total_time=600 \
    test/harness/corpus/
```

`-fork=N` spawns N libFuzzer children that mutate independently and
merge corpus entries through the shared directory; a crash in one
child gets isolated and saved to `crash-<sha1>` without taking the
others down. `-jobs=N -workers=M` is the older equivalent (N total
batches, M concurrent workers) and writes one log per job to the
working directory; prefer `-fork` unless you specifically want the
log-per-job layout.

Useful flags during a run:

- `-max_len=8192` raise the per-input cap from the default 4096.
- `-seed=1` deterministic mutator seed for reproducing a session.
- `-only_ascii=1` constrain to printable bytes (rarely useful here).
- A bare path argument re-runs that single input through
  `LLVMFuzzerTestOneInput`, useful when bisecting a saved crash.

## WAYLAND_DEBUG

The optimization lever for nomonad is request churn per manage / render
cycle, not CPU. To inspect:

```sh
WAYLAND_DEBUG=1 build/nomonad 2>&1 | tee /tmp/wd.log
```

Count requests by name:

```sh
awk '/-> river_/ {print $3}' /tmp/wd.log | sort | uniq -c | sort -rn
```

What to look for:

- Steady-state churn. Holding a key (focus-cycle, swap) on a populated
  workspace should produce a small handful of requests per press: one
  `river_seat_v1.focus_window`, one `river_node_v1.place_top`, plus a
  manage_dirty / manage_finish pair. Per-window protocol caches
  (`set_position`, `propose_dimensions`, `set_borders`,
  `set_dimension_bounds`, `show` / `hide`) should be silent unless the
  geometry actually changed. Anything that scales with window count on
  an idempotent action is a regression.
- Resize / move loops. While dragging a floating window,
  `propose_dimensions` should fire at the input event rate, not faster.
  The `last_proposed_w/h` cache in `struct Window` exists to break the
  propose -> dimensions -> manage_start feedback loop.
- First-frame cost. Spawning windows is expected to be loud (set_*,
  borders, decoration, bounds). What matters is that subsequent cycles
  on the same set are quiet.

## CPU profiling

After `make perf`:

```sh
perf record -F 99 --call-graph dwarf -g -- build-perf/nomonad
perf report
```

Or flamegraphs (needs Brendan Gregg's
[FlameGraph](https://github.com/brendangregg/FlameGraph) on PATH):

```sh
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

`-fno-omit-frame-pointer` is what makes `--call-graph dwarf` produce
honest stacks; that is what `make perf` adds on top of `-O2`.
