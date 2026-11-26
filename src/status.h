/* SPDX-License-Identifier: 0BSD */
/*
 * Status broadcast IPC. A unix-domain socket at
 * $XDG_RUNTIME_DIR/nomonad-status.sock (fallback
 * /tmp/nomonad-status-$UID.sock) accepts connections from any
 * number of read-only subscribers and streams logfmt snapshots of
 * publishable WM state on every cycle that changes it.
 *
 * Designed for waybar / dunst hooks / a libnotify wrapper that
 * needs "what workspace is window with PID X on" without polling.
 * One-way: clients only read. There is no command surface here;
 * see todo.org for the planned nomonad-ctl request/response side.
 *
 * Wire format (newline-delimited records, multiple records per
 * frame):
 *
 *   type=frame_start seq=<n> time_start_unix_ns=<u64>
 *   type=output index=<i> visible_workspace=<n>
 *   type=workspace index=<i> focused_window=<id> layout=<name>
 *   type=window id=<id> workspace=<i> app_id=<value>
 *           [title=<value>] focused=<0|1> floating=<0|1>
 *           fullscreen=<0|1> [pid=<n>]
 *   type=frame_end seq=<n> time_end_unix_ns=<u64>
 *
 * The two timestamps come from CLOCK_REALTIME (vDSO, ~25 ns each
 * on x86_64) sampled at the top of wm_handle_manage_start and
 * right before status_publish. They bracket the manage cycle the
 * subscriber's frame describes. unix_ns is the standard wall-
 * clock reference: correlates with journald / syslog / date, fits
 * in a u64, easy to divide for ms or seconds.
 *
 * <id> is the river_window_v1.identifier when available (protocol
 * v4+, persistent across the window's lifetime), falling back to a
 * monotonic counter assigned the first time we publish a window.
 * Values are unquoted unless they contain space, =, " or backslash;
 * those are wrapped in double quotes with " and \ escaped.
 *
 * Privacy gates (consulted at status_init):
 *   NM_STATUS_TITLES   on/off (default off; titles can leak URLs,
 *                      file paths, recently-typed search queries)
 *   NM_STATUS_APPID    on/off (default on; app_id is the wayland
 *                      class equivalent and rarely sensitive)
 *   NM_STATUS_PID      on/off (default off; exposes
 *                      unreliable_pid event surface)
 *   NM_STATUS_DEBUG    on/off (stderr line per publish: byte
 *                      count, subscriber count, hit/miss on hash)
 *
 * Lifecycle:
 *   status_init       once at startup, after wm_init. Returns -1
 *                     on failure; the WM keeps running without
 *                     IPC. status_init is idempotent on failure.
 *   status_fill_pollfds + status_handle_events
 *                     wired into the main poll loop. The server
 *                     fd plus each subscriber fd live in the
 *                     same pollset as the wayland / signal /
 *                     inotify fds. No threads.
 *   status_publish    called from wm_handle_manage_start after
 *                     the layout / status gates resolve. Skipped
 *                     when status_observed is false; further
 *                     deduped by an FNV-1a hash over the snapshot
 *                     bytes so quiet sessions cost effectively
 *                     nothing once initialised.
 *   status_shutdown   atexit; closes server fd, drops clients,
 *                     unlinks the socket path.
 *
 * Failure isolation: any write error on a subscriber fd marks it
 * for drop; the WM cycle is unaffected. Bound at NM_STATUS_MAX_SUBS
 * (16) so a hostile / forgetful subscriber cannot exhaust fds.
 */

#ifndef NOMONAD_STATUS_H
#define NOMONAD_STATUS_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#include "nomonad.h"

int status_init(void);
void status_shutdown(void);

/*
 * Append at most `cap` pollfd entries to `out` (server fd + per-
 * subscriber fds). Returns the number of entries written. Skips
 * everything when status_init was not called or failed.
 */
size_t status_fill_pollfds(struct pollfd *out, size_t cap);

/*
 * Process readiness on the slice of pollfd entries previously
 * filled by status_fill_pollfds. accept(2)s on the server fd and
 * drops subscribers that signal POLLERR / POLLHUP / EOF.
 */
void status_handle_events(struct pollfd *fds, size_t n);

/*
 * Build, hash-dedupe, and broadcast a snapshot of current WM
 * state. Pass true when the cycle observed a publishable change
 * (status-dirty edge); false skips the build entirely. Safe to
 * call when status_init failed; becomes a no-op. start_unix_ns
 * and end_unix_ns are the CLOCK_REALTIME bracket of the manage
 * cycle this snapshot represents; they are emitted on
 * frame_start / frame_end and do not enter the body hash.
 */
void status_publish(bool status_dirty, u64 start_unix_ns, u64 end_unix_ns);

#endif /* NOMONAD_STATUS_H */
