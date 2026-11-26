/* SPDX-License-Identifier: 0BSD */
/*
 * Per-cycle wall-clock instrumentation for production nomonad. Gated
 * by NM_TIMING; if the env var is unset, timing_init returns 0 and
 * every timing_* call is a cheap no-op.
 *
 * One line per manage / render cycle written to NM_TIMING:
 *   T <ns> manage_us=<m> render_us=<r> windows=<w> dirty=<0|1>
 *
 * dirty=1 means the cycle observed a publishable state change
 * (wm.dirty_status was set somewhere between the prior manage_start
 * and this one's read+clear). dirty=0 marks a cycle that *would*
 * have been a no-op under the manage-cycle dirty gating.
 *
 * The brackets bracket the WM-side cycle bodies in wm.c, not the
 * surrounding wl_display_dispatch_pending. They time the part of the
 * cycle nomonad is responsible for: manage_start handler entry to
 * manage_finish request, then render_start handler entry to
 * render_finish request. Server-side latency is not measured here;
 * the recorder + replayer cover that workflow separately.
 *
 * Emit happens at the end of timing_end_render so a single line
 * carries both halves of the cycle. timing_begin_manage and
 * timing_begin_render reset the relevant origin; timing_end_manage
 * stashes the elapsed for the upcoming emit.
 *
 * Output is stdio block-buffered (64 KB). Per-event syscall overhead
 * amortises to ~once per ~1000 cycles. atexit fires timing_shutdown
 * on any normal exit. SIGTERM-killed sessions lose at most the last
 * buffered batch.
 *
 * Lifecycle: call timing_init once at startup after wm_init. Pair
 * with timing_shutdown via atexit, which is installed on success.
 *
 * Single-threaded: nomonad's manage cycle is too. No locking.
 */

#ifndef NOMONAD_TIMING_H
#define NOMONAD_TIMING_H

#include "nomonad.h"

int timing_init(void);
void timing_shutdown(void);

void timing_begin_manage(void);
void timing_end_manage(void);
void timing_begin_render(void);
void timing_end_render(void);

/*
 * Stash the cycle's dirty observation so timing_end_render can include
 * it on the emitted line. Called once per cycle, near the end of
 * wm_handle_manage_start (after seat_manage, before manage_finish), so
 * the value reflects every dirty source that fired during the cycle.
 * No-op when timing is disabled.
 */
void timing_observe_dirty(bool dirty);

#endif /* NOMONAD_TIMING_H */
