/* SPDX-License-Identifier: 0BSD */
/*
 * Session-trace recorder. Gated by NM_RECORD; if the env var is unset,
 * recorder_init returns 0 and every recorder_event_* call is a cheap
 * no-op. See docs/session-format.md for the trace format and the
 * NM_RECORD_TITLES / NM_RECORD_APPID / NM_RECORD_POINTER privacy gates.
 *
 * Lifecycle: call recorder_init once, after wm_init() but before
 * registering wm_listener so the river-side initial-state events
 * (output / seat / window) flow through the recorder. Pair with
 * recorder_shutdown at exit (also installed via atexit).
 *
 * The recorder is single-threaded; nomonad's manage cycle is too, so
 * no synchronisation is needed. The output FILE is line-buffered, so
 * each event hits the kernel page cache as soon as its terminating
 * newline is written. atexit fires recorder_shutdown for any normal
 * exit; SIGTERM-killed sessions lose at most the in-flight line.
 */

#ifndef NOMONAD_RECORDER_H
#define NOMONAD_RECORDER_H

#include <stdbool.h>
#include <stdio.h>

#include "nomonad.h"

int recorder_init(void);
void recorder_shutdown(void);

void recorder_event_config_set(const char *text);
void recorder_event_config_reload(const char *text);

void recorder_event_output_add(struct Output *output);
void recorder_event_output_position(struct Output *output);
void recorder_event_output_dimensions(struct Output *output);
void recorder_event_output_remove(struct Output *output);
void recorder_event_output_usable(struct Output *output);
FILE *recorder_open_private(const char *path);
void recorder_event_layer_focus(struct Seat *seat);
void recorder_output_freed(struct Output *output);

void recorder_event_seat_add(struct Seat *seat);
void recorder_event_seat_remove(struct Seat *seat);
void recorder_seat_freed(struct Seat *seat);

void recorder_event_window_add(struct Window *window);
void recorder_event_window_app_id(struct Window *window);
void recorder_event_window_title(struct Window *window);
void recorder_event_window_parent(struct Window *window,
				  struct Window *parent);
void recorder_event_window_dimensions_hint(struct Window *window);
void recorder_event_window_fullscreen(struct Window *window, bool on);
void recorder_event_window_close(struct Window *window);
void recorder_window_freed(struct Window *window);

void recorder_event_pointer_to(struct Seat *seat, struct Output *output);
void recorder_event_pointer_position(struct Seat *seat, s32 x, s32 y);
void recorder_event_pointer_enter(struct Seat *seat, struct Window *window);
void recorder_event_pointer_leave(struct Seat *seat);
void recorder_event_op_delta(struct Seat *seat, s32 dx, s32 dy);
void recorder_event_op_release(struct Seat *seat);
void recorder_event_window_interaction(struct Seat *seat,
				       struct Window *window);
void recorder_event_ate_unbound(struct Seat *seat);

void recorder_event_press(struct Seat *seat, const struct Action *action);
void recorder_event_pointer_press(struct Seat *seat,
				  const struct Action *action);
void recorder_event_repeat_press(struct Seat *seat,
				 const struct Action *action);
void recorder_event_stop_repeat(struct Seat *seat);

void recorder_event_session_locked(void);
void recorder_event_session_unlocked(void);

#endif /* NOMONAD_RECORDER_H */
