/* SPDX-License-Identifier: 0BSD */
/*
 * Public surface of the headless test harness.
 *
 * harness_lib.c owns the main.c-equivalent globals (wm, g_config,
 * window_manager_v1, xkb_bindings_v1, g_reload_pending), the staged
 * config slot, the schedule_reload / reload_now stubs, the
 * fake_proxy mirror, the structural / layout invariants run after
 * every cycle, and the injection + cycle API exposed here.
 *
 * Two consumers link against harness_lib.o:
 *   - test/harness/harness.c: scenarios + runner main, drives the
 *     harness from hand-written sequences.
 *   - test/harness/fuzz.c: LLVMFuzzerTestOneInput, drives the harness
 *     from libFuzzer-supplied byte buffers.
 */

#ifndef HARNESS_LIB_H
#define HARNESS_LIB_H

#include <stdint.h>
#include <stdio.h>

#include "binding.h"
#include "config.h"
#include "nomonad.h"

struct wl_proxy;

/*
 * Staged config slot for in-memory reload. harness_stage_reload sets
 * it; the harness reload_now (called from wm_handle_manage_start when
 * g_reload_pending is set) consumes it. NULL means no reload pending.
 */
extern struct Config *g_staged_config;

void harness_init(const char *config_src);
void harness_teardown(void);

struct Output *harness_add_output(int x, int y, int w, int h);
struct Seat *harness_add_seat(struct Output *primary);
struct Window *harness_add_window(const char *app_id);

void harness_close_window(struct Window *w);
void harness_remove_output(struct Output *out);
void harness_stage_reload(const char *config_src);
void harness_press(struct Seat *s, enum ActionKind kind, const char *arg);
void harness_key_press(struct Seat *s, enum ActionKind kind);
void harness_enable_layer_shell(void);
void harness_interact(struct Seat *s, struct Window *w);
void harness_pointer_at(struct Seat *s, s32 x, s32 y);
void harness_window_capture(struct Window *w, u32 count);
void harness_output_capture(struct Output *o, u32 count);
void harness_window_presentation(struct Window *w, u32 hint);
void *harness_input_device(u32 type);
void *harness_libinput_device(s32 tap_fingers);
void harness_libinput_defaults(void *dev, u32 tap, u32 natural_scroll);
void *harness_enable_xkb_config(void);
void *harness_xkb_keyboard(void *xkb_config);
void harness_keymap_success(void *keymap);
void harness_keymap_failure(void *keymap);
void harness_finish_tracked(void);
uint32_t harness_last_opcode(void);
void harness_track_iface(const char *name);
size_t harness_tracked_count(void);
struct wl_proxy *harness_tracked(size_t i);
void harness_set_usable(struct Output *o, s32 x, s32 y, s32 w, s32 h);
void harness_layer_focus(struct Seat *s, enum LayerFocus focus);
void harness_key_release(struct Seat *s, enum ActionKind kind);
void harness_pointer_to(struct Seat *s, struct Output *out);
void harness_pointer_enter(struct Seat *s, struct Window *w);
void harness_pointer_leave(struct Seat *s);
void harness_op_delta(struct Seat *s, int dx, int dy);
void harness_op_release(struct Seat *s);
void harness_cycle(void);

int harness_count_ws_windows(int wsi);

/*
 * Wire-message counter from fakewl.c. Every
 * libwayland-client wl_proxy_marshal_flags is one wire-protocol
 * message; bench scenarios diff this across cycles to measure the
 * "requests per cycle" metric the README calls out.
 */
uint64_t harness_request_count_get(void);
void harness_request_count_reset(void);

/*
 * Per-cycle stats: wall-clock timing (CLOCK_MONOTONIC) and request
 * count. Off by default. harness_stats_enable starts a fresh window
 * (resets request counter, drops any prior samples). Each subsequent
 * harness_cycle records one sample. harness_stats_print sorts and
 * prints min / mean / p50 / p95 / p99 / max for both metrics.
 *
 * harness_set_invariants(false) skips the structural / layout
 * assertions inside harness_cycle; the assertions are O(n^2) on
 * worst-case workspaces and would otherwise dominate timing
 * measurements that try to isolate production code cost.
 */
void harness_stats_enable(void);
void harness_stats_print(FILE *out);
void harness_set_invariants(bool enabled);

#endif /* HARNESS_LIB_H */
