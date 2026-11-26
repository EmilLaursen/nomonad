/* SPDX-License-Identifier: 0BSD */
/*
 * Derived from tinyrwm.c, (c) 2026 Isaac Freund, 0BSD.
 *
 * Per-seat binding lifecycle: pressed/released callbacks, owning the
 * river_xkb_binding_v1 / river_pointer_binding_v1 objects, and creating
 * them from a Config produced by config.c.
 */

#include <stdlib.h>
#include <string.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>

#include <xkbcommon/xkbcommon.h>

#include "nomonad.h"
#include "action.h"
#include "binding.h"
#include "config.h"
#include "recorder.h"

/* ---- xkb binding ------------------------------------------------------ */

static void xkb_binding_handle_pressed(void *data,
				       struct river_xkb_binding_v1 *obj)
{
	struct XkbBinding *binding = data;
	action_clear(&binding->seat->pending_action);
	binding->seat->pending_action = action_dup(&binding->action);
	binding->seat->pending_mods = binding->mods;
	recorder_event_press(binding->seat, &binding->action);
	/*
	 * river only fires `pressed` once per physical press; auto-repeat
	 * is the WM's job (see river_xkb_binding_v1.stop_repeat). Single
	 * repeat slot per seat: a fresh press replaces any in-flight
	 * repeat from a prior binding.
	 */
	if (action_kind_repeats(binding->action.kind))
		seat_repeat_arm(binding->seat, &binding->action);
	else
		seat_repeat_disarm(binding->seat);
}

static void xkb_binding_handle_released(void *data,
					struct river_xkb_binding_v1 *obj)
{
	struct XkbBinding *binding = data;
	seat_repeat_disarm(binding->seat);
}

static void xkb_binding_handle_stop_repeat(void *data,
					   struct river_xkb_binding_v1 *obj)
{
	struct XkbBinding *binding = data;
	recorder_event_stop_repeat(binding->seat);
	seat_repeat_disarm(binding->seat);
}

static const struct river_xkb_binding_v1_listener river_xkb_binding_listener = {
	.pressed = xkb_binding_handle_pressed,
	.released = xkb_binding_handle_released,
	.stop_repeat = xkb_binding_handle_stop_repeat,
};

static void xkb_binding_destroy(struct XkbBinding *binding)
{
	river_xkb_binding_v1_destroy(binding->obj);
	action_clear(&binding->action);
	wl_list_remove(&binding->link);
	free(binding);
}

static void xkb_binding_create(struct Seat *seat, u32 mods, xkb_keysym_t keysym,
			       const struct Action *action)
{
	struct XkbBinding *binding = calloc(1, sizeof(*binding));

	if (binding == NULL)
		return;
	binding->obj = river_xkb_bindings_v1_get_xkb_binding(
		xkb_bindings_v1, seat->obj, keysym, mods);
	binding->seat = seat;
	binding->action = action_dup(action);
	binding->mods = mods;

	river_xkb_binding_v1_add_listener(binding->obj,
					  &river_xkb_binding_listener, binding);
	river_xkb_binding_v1_enable(binding->obj);

	wl_list_insert(seat->xkb_bindings.prev, &binding->link);
}

/* ---- pointer binding -------------------------------------------------- */

static void pointer_binding_handle_pressed(void *data,
					   struct river_pointer_binding_v1 *obj)
{
	struct PointerBinding *binding = data;
	action_clear(&binding->seat->pending_action);
	binding->seat->pending_action = action_dup(&binding->action);
	recorder_event_pointer_press(binding->seat, &binding->action);
}

static void
pointer_binding_handle_released(void *data,
				struct river_pointer_binding_v1 *obj)
{
}

static const struct river_pointer_binding_v1_listener
	river_pointer_binding_listener = {
		.pressed = pointer_binding_handle_pressed,
		.released = pointer_binding_handle_released,
	};

static void pointer_binding_destroy(struct PointerBinding *binding)
{
	river_pointer_binding_v1_destroy(binding->obj);
	action_clear(&binding->action);
	wl_list_remove(&binding->link);
	free(binding);
}

static void pointer_binding_create(struct Seat *seat, u32 mods, u32 button,
				   const struct Action *action)
{
	struct PointerBinding *binding = calloc(1, sizeof(*binding));

	if (binding == NULL)
		return;
	binding->obj =
		river_seat_v1_get_pointer_binding(seat->obj, button, mods);
	binding->seat = seat;
	binding->action = action_dup(action);

	river_pointer_binding_v1_add_listener(
		binding->obj, &river_pointer_binding_listener, binding);
	river_pointer_binding_v1_enable(binding->obj);

	wl_list_insert(seat->pointer_bindings.prev, &binding->link);
}

/* ---- public API ------------------------------------------------------- */

void binding_register_from_config(struct Seat *seat, const struct Config *cfg)
{
	if (cfg == NULL)
		return;
	for (size_t i = 0; i < cfg->n_xkb; i++) {
		const struct XkbBindingDef *d = &cfg->xkb[i];
		xkb_binding_create(seat, d->mods, d->keysym, &d->action);
	}
	for (size_t i = 0; i < cfg->n_ptr; i++) {
		const struct PointerBindingDef *d = &cfg->ptr[i];
		pointer_binding_create(seat, d->mods, d->button, &d->action);
	}
}

void binding_register_submap(struct Seat *seat, const struct SubmapDef *def)
{
	if (def == NULL)
		return;
	for (size_t i = 0; i < def->n_entries; i++) {
		const struct SubmapEntry *e = &def->entries[i];
		xkb_binding_create(seat, 0, e->keysym, &e->action);
	}
}

void binding_seat_cleanup(struct Seat *seat)
{
	struct PointerBinding *pointer_binding, *pointer_binding_tmp;
	struct XkbBinding *xkb_binding, *xkb_binding_tmp;

	seat_repeat_disarm(seat);
	wl_list_for_each_safe(xkb_binding, xkb_binding_tmp, &seat->xkb_bindings,
			      link) {
		xkb_binding_destroy(xkb_binding);
	}

	wl_list_for_each_safe(pointer_binding, pointer_binding_tmp,
			      &seat->pointer_bindings, link) {
		pointer_binding_destroy(pointer_binding);
	}
}
