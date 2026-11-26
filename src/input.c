/* SPDX-License-Identifier: 0BSD */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-input-management-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>

#include "config.h"
#include "input.h"
#include "keymap.h"
#include "nomonad.h"

#define INPUT_VERSION_MAX 2

#define NM_RIVER_REPEAT_RATE 40
#define NM_RIVER_REPEAT_DELAY 400

enum LiSetting {
	LI_TAP,
	LI_NATURAL_SCROLL,
	LI_DWT,
	LI_MIDDLE_EMULATION,
	LI_N,
};

struct InputDevice {
	struct river_input_device_v1 *obj;
	bool repeat_applied;
	bool keyboard;
	struct wl_list link;
};

struct LibinputDevice {
	struct river_libinput_device_v1 *obj;
	bool applied[LI_N];
	s32 def[LI_N];
	bool touchpad;
	struct wl_list link;
};

struct river_input_manager_v1 *input_manager_v1;
struct river_libinput_config_v1 *input_libinput_v1;

static struct wl_list g_devices = { &g_devices, &g_devices };
static struct wl_list g_li_devices = { &g_li_devices, &g_li_devices };

static void result_done(void *data, struct river_libinput_result_v1 *obj)
{
	river_libinput_result_v1_destroy(obj);
}

static const struct river_libinput_result_v1_listener result_listener = {
	.success = result_done,
	.unsupported = result_done,
	.invalid = result_done,
};

static void track_result(struct river_libinput_result_v1 *result)
{
	if (result != NULL)
		river_libinput_result_v1_add_listener(result, &result_listener,
						      NULL);
}

static void apply_repeat(struct InputDevice *d)
{
	const struct InputConfig *ic;

	if (g_config == NULL || !d->keyboard)
		return;
	ic = &g_config->input;
	if (ic->repeat_set) {
		river_input_device_v1_set_repeat_info(d->obj, ic->repeat_rate,
						      ic->repeat_delay);
		d->repeat_applied = true;
	} else if (d->repeat_applied) {
		river_input_device_v1_set_repeat_info(
			d->obj, NM_RIVER_REPEAT_RATE, NM_RIVER_REPEAT_DELAY);
		d->repeat_applied = false;
	}
}

static u32 li_state(enum LiSetting s, bool on)
{
	switch (s) {
	case LI_TAP:
		return on ? RIVER_LIBINPUT_DEVICE_V1_TAP_STATE_ENABLED :
			    RIVER_LIBINPUT_DEVICE_V1_TAP_STATE_DISABLED;
	case LI_NATURAL_SCROLL:
		return on ? RIVER_LIBINPUT_DEVICE_V1_NATURAL_SCROLL_STATE_ENABLED :
			    RIVER_LIBINPUT_DEVICE_V1_NATURAL_SCROLL_STATE_DISABLED;
	case LI_DWT:
		return on ? RIVER_LIBINPUT_DEVICE_V1_DWT_STATE_ENABLED :
			    RIVER_LIBINPUT_DEVICE_V1_DWT_STATE_DISABLED;
	default:
		return on ? RIVER_LIBINPUT_DEVICE_V1_MIDDLE_EMULATION_STATE_ENABLED :
			    RIVER_LIBINPUT_DEVICE_V1_MIDDLE_EMULATION_STATE_DISABLED;
	}
}

static void li_send(struct river_libinput_device_v1 *o, enum LiSetting s,
		    u32 state)
{
	switch (s) {
	case LI_TAP:
		track_result(river_libinput_device_v1_set_tap(o, state));
		break;
	case LI_NATURAL_SCROLL:
		track_result(
			river_libinput_device_v1_set_natural_scroll(o, state));
		break;
	case LI_DWT:
		track_result(river_libinput_device_v1_set_dwt(o, state));
		break;
	default:
		track_result(river_libinput_device_v1_set_middle_emulation(
			o, state));
		break;
	}
}

static void apply_touchpad(struct LibinputDevice *d)
{
	const struct InputConfig *ic;
	int want[LI_N];

	if (g_config == NULL || !d->touchpad)
		return;
	ic = &g_config->input;
	want[LI_TAP] = ic->tap;
	want[LI_NATURAL_SCROLL] = ic->natural_scroll;
	want[LI_DWT] = ic->dwt;
	want[LI_MIDDLE_EMULATION] = ic->middle_emulation;
	for (int s = 0; s < LI_N; s++) {
		if (want[s] >= 0) {
			li_send(d->obj, (enum LiSetting)s,
				li_state((enum LiSetting)s, want[s] != 0));
			d->applied[s] = true;
		} else if (d->applied[s] && d->def[s] >= 0) {
			li_send(d->obj, (enum LiSetting)s, (u32)d->def[s]);
			d->applied[s] = false;
		}
	}
}

static void input_device_removed(void *data, struct river_input_device_v1 *obj)
{
	struct InputDevice *d = data;

	river_input_device_v1_destroy(d->obj);
	wl_list_remove(&d->link);
	free(d);
}

static void input_device_type(void *data, struct river_input_device_v1 *obj,
			      u32 type)
{
	struct InputDevice *d = data;

	d->keyboard = (type == RIVER_INPUT_DEVICE_V1_TYPE_KEYBOARD);
	apply_repeat(d);
}

static void input_device_name(void *data, struct river_input_device_v1 *obj,
			      const char *name)
{
}

static void input_device_done(void *data, struct river_input_device_v1 *obj)
{
}

static const struct river_input_device_v1_listener input_device_listener = {
	.removed = input_device_removed,
	.type = input_device_type,
	.name = input_device_name,
	.done = input_device_done,
};

static void input_manager_finished(void *data,
				   struct river_input_manager_v1 *obj)
{
}

static void input_manager_input_device(void *data,
				       struct river_input_manager_v1 *obj,
				       struct river_input_device_v1 *id)
{
	struct InputDevice *d = calloc(1, sizeof(*d));

	if (d == NULL) {
		river_input_device_v1_destroy(id);
		return;
	}
	d->obj = id;
	river_input_device_v1_add_listener(id, &input_device_listener, d);
	wl_list_insert(g_devices.prev, &d->link);
}

const struct river_input_manager_v1_listener input_manager_listener = {
	.finished = input_manager_finished,
	.input_device = input_manager_input_device,
};

static void input_li_removed(void *data, struct river_libinput_device_v1 *obj)
{
	struct LibinputDevice *d = data;

	river_libinput_device_v1_destroy(d->obj);
	wl_list_remove(&d->link);
	free(d);
}

static void input_li_input_device(void *data,
				  struct river_libinput_device_v1 *obj,
				  struct river_input_device_v1 *device)
{
}

static void input_li_tap_support(void *data,
				 struct river_libinput_device_v1 *obj,
				 s32 finger_count)
{
	struct LibinputDevice *d = data;

	d->touchpad = finger_count > 0;
	apply_touchpad(d);
}

static void input_li_send_events_support(void *data,
					 struct river_libinput_device_v1 *obj,
					 u32 modes)
{
}

static void input_li_send_events_default(void *data,
					 struct river_libinput_device_v1 *obj,
					 u32 mode)
{
}

static void input_li_send_events_current(void *data,
					 struct river_libinput_device_v1 *obj,
					 u32 mode)
{
}

static void input_li_tap_default(void *data,
				 struct river_libinput_device_v1 *obj,
				 u32 state)
{
	struct LibinputDevice *d = data;

	d->def[LI_TAP] = (s32)state;
}

static void input_li_tap_current(void *data,
				 struct river_libinput_device_v1 *obj,
				 u32 state)
{
}

static void input_li_tap_button_map_default(
	void *data, struct river_libinput_device_v1 *obj, u32 button_map)
{
}

static void input_li_tap_button_map_current(
	void *data, struct river_libinput_device_v1 *obj, u32 button_map)
{
}

static void input_li_drag_default(void *data,
				  struct river_libinput_device_v1 *obj,
				  u32 state)
{
}

static void input_li_drag_current(void *data,
				  struct river_libinput_device_v1 *obj,
				  u32 state)
{
}

static void input_li_drag_lock_default(void *data,
				       struct river_libinput_device_v1 *obj,
				       u32 state)
{
}

static void input_li_drag_lock_current(void *data,
				       struct river_libinput_device_v1 *obj,
				       u32 state)
{
}

static void input_li_three_finger_drag_support(
	void *data, struct river_libinput_device_v1 *obj, s32 finger_count)
{
}

static void input_li_three_finger_drag_default(
	void *data, struct river_libinput_device_v1 *obj, u32 state)
{
}

static void input_li_three_finger_drag_current(
	void *data, struct river_libinput_device_v1 *obj, u32 state)
{
}

static void input_li_calibration_matrix_support(
	void *data, struct river_libinput_device_v1 *obj, s32 supported)
{
}

static void
input_li_calibration_matrix_default(void *data,
				    struct river_libinput_device_v1 *obj,
				    struct wl_array *matrix)
{
}

static void
input_li_calibration_matrix_current(void *data,
				    struct river_libinput_device_v1 *obj,
				    struct wl_array *matrix)
{
}

static void input_li_accel_profiles_support(
	void *data, struct river_libinput_device_v1 *obj, u32 profiles)
{
}

static void input_li_accel_profile_default(void *data,
					   struct river_libinput_device_v1 *obj,
					   u32 profile)
{
}

static void input_li_accel_profile_current(void *data,
					   struct river_libinput_device_v1 *obj,
					   u32 profile)
{
}

static void input_li_accel_speed_default(void *data,
					 struct river_libinput_device_v1 *obj,
					 struct wl_array *speed)
{
}

static void input_li_accel_speed_current(void *data,
					 struct river_libinput_device_v1 *obj,
					 struct wl_array *speed)
{
}

static void input_li_natural_scroll_support(
	void *data, struct river_libinput_device_v1 *obj, s32 supported)
{
}

static void
input_li_natural_scroll_default(void *data,
				struct river_libinput_device_v1 *obj, u32 state)
{
	struct LibinputDevice *d = data;

	d->def[LI_NATURAL_SCROLL] = (s32)state;
}

static void
input_li_natural_scroll_current(void *data,
				struct river_libinput_device_v1 *obj, u32 state)
{
}

static void input_li_left_handed_support(void *data,
					 struct river_libinput_device_v1 *obj,
					 s32 supported)
{
}

static void input_li_left_handed_default(void *data,
					 struct river_libinput_device_v1 *obj,
					 u32 state)
{
}

static void input_li_left_handed_current(void *data,
					 struct river_libinput_device_v1 *obj,
					 u32 state)
{
}

static void input_li_click_method_support(void *data,
					  struct river_libinput_device_v1 *obj,
					  u32 methods)
{
}

static void input_li_click_method_default(void *data,
					  struct river_libinput_device_v1 *obj,
					  u32 method)
{
}

static void input_li_click_method_current(void *data,
					  struct river_libinput_device_v1 *obj,
					  u32 method)
{
}

static void input_li_clickfinger_button_map_default(
	void *data, struct river_libinput_device_v1 *obj, u32 button_map)
{
}

static void input_li_clickfinger_button_map_current(
	void *data, struct river_libinput_device_v1 *obj, u32 button_map)
{
}

static void input_li_middle_emulation_support(
	void *data, struct river_libinput_device_v1 *obj, s32 supported)
{
}

static void input_li_middle_emulation_default(
	void *data, struct river_libinput_device_v1 *obj, u32 state)
{
	struct LibinputDevice *d = data;

	d->def[LI_MIDDLE_EMULATION] = (s32)state;
}

static void input_li_middle_emulation_current(
	void *data, struct river_libinput_device_v1 *obj, u32 state)
{
}

static void input_li_scroll_method_support(void *data,
					   struct river_libinput_device_v1 *obj,
					   u32 methods)
{
}

static void input_li_scroll_method_default(void *data,
					   struct river_libinput_device_v1 *obj,
					   u32 method)
{
}

static void input_li_scroll_method_current(void *data,
					   struct river_libinput_device_v1 *obj,
					   u32 method)
{
}

static void input_li_scroll_button_default(void *data,
					   struct river_libinput_device_v1 *obj,
					   u32 button)
{
}

static void input_li_scroll_button_current(void *data,
					   struct river_libinput_device_v1 *obj,
					   u32 button)
{
}

static void input_li_scroll_button_lock_default(
	void *data, struct river_libinput_device_v1 *obj, u32 state)
{
}

static void input_li_scroll_button_lock_current(
	void *data, struct river_libinput_device_v1 *obj, u32 state)
{
}

static void input_li_dwt_support(void *data,
				 struct river_libinput_device_v1 *obj,
				 s32 supported)
{
}

static void input_li_dwt_default(void *data,
				 struct river_libinput_device_v1 *obj,
				 u32 state)
{
	struct LibinputDevice *d = data;

	d->def[LI_DWT] = (s32)state;
}

static void input_li_dwt_current(void *data,
				 struct river_libinput_device_v1 *obj,
				 u32 state)
{
}

static void input_li_dwtp_support(void *data,
				  struct river_libinput_device_v1 *obj,
				  s32 supported)
{
}

static void input_li_dwtp_default(void *data,
				  struct river_libinput_device_v1 *obj,
				  u32 state)
{
}

static void input_li_dwtp_current(void *data,
				  struct river_libinput_device_v1 *obj,
				  u32 state)
{
}

static void input_li_rotation_support(void *data,
				      struct river_libinput_device_v1 *obj,
				      s32 supported)
{
}

static void input_li_rotation_default(void *data,
				      struct river_libinput_device_v1 *obj,
				      u32 angle)
{
}

static void input_li_rotation_current(void *data,
				      struct river_libinput_device_v1 *obj,
				      u32 angle)
{
}

static void input_li_done(void *data, struct river_libinput_device_v1 *obj)
{
}

static const struct river_libinput_device_v1_listener input_li_listener = {
	.removed = input_li_removed,
	.input_device = input_li_input_device,
	.send_events_support = input_li_send_events_support,
	.send_events_default = input_li_send_events_default,
	.send_events_current = input_li_send_events_current,
	.tap_support = input_li_tap_support,
	.tap_default = input_li_tap_default,
	.tap_current = input_li_tap_current,
	.tap_button_map_default = input_li_tap_button_map_default,
	.tap_button_map_current = input_li_tap_button_map_current,
	.drag_default = input_li_drag_default,
	.drag_current = input_li_drag_current,
	.drag_lock_default = input_li_drag_lock_default,
	.drag_lock_current = input_li_drag_lock_current,
	.three_finger_drag_support = input_li_three_finger_drag_support,
	.three_finger_drag_default = input_li_three_finger_drag_default,
	.three_finger_drag_current = input_li_three_finger_drag_current,
	.calibration_matrix_support = input_li_calibration_matrix_support,
	.calibration_matrix_default = input_li_calibration_matrix_default,
	.calibration_matrix_current = input_li_calibration_matrix_current,
	.accel_profiles_support = input_li_accel_profiles_support,
	.accel_profile_default = input_li_accel_profile_default,
	.accel_profile_current = input_li_accel_profile_current,
	.accel_speed_default = input_li_accel_speed_default,
	.accel_speed_current = input_li_accel_speed_current,
	.natural_scroll_support = input_li_natural_scroll_support,
	.natural_scroll_default = input_li_natural_scroll_default,
	.natural_scroll_current = input_li_natural_scroll_current,
	.left_handed_support = input_li_left_handed_support,
	.left_handed_default = input_li_left_handed_default,
	.left_handed_current = input_li_left_handed_current,
	.click_method_support = input_li_click_method_support,
	.click_method_default = input_li_click_method_default,
	.click_method_current = input_li_click_method_current,
	.clickfinger_button_map_default =
		input_li_clickfinger_button_map_default,
	.clickfinger_button_map_current =
		input_li_clickfinger_button_map_current,
	.middle_emulation_support = input_li_middle_emulation_support,
	.middle_emulation_default = input_li_middle_emulation_default,
	.middle_emulation_current = input_li_middle_emulation_current,
	.scroll_method_support = input_li_scroll_method_support,
	.scroll_method_default = input_li_scroll_method_default,
	.scroll_method_current = input_li_scroll_method_current,
	.scroll_button_default = input_li_scroll_button_default,
	.scroll_button_current = input_li_scroll_button_current,
	.scroll_button_lock_default = input_li_scroll_button_lock_default,
	.scroll_button_lock_current = input_li_scroll_button_lock_current,
	.dwt_support = input_li_dwt_support,
	.dwt_default = input_li_dwt_default,
	.dwt_current = input_li_dwt_current,
	.dwtp_support = input_li_dwtp_support,
	.dwtp_default = input_li_dwtp_default,
	.dwtp_current = input_li_dwtp_current,
	.rotation_support = input_li_rotation_support,
	.rotation_default = input_li_rotation_default,
	.rotation_current = input_li_rotation_current,
	.done = input_li_done,
};

static void input_libinput_finished(void *data,
				    struct river_libinput_config_v1 *obj)
{
}

static void input_libinput_device(void *data,
				  struct river_libinput_config_v1 *obj,
				  struct river_libinput_device_v1 *id)
{
	struct LibinputDevice *d = calloc(1, sizeof(*d));

	if (d == NULL) {
		river_libinput_device_v1_destroy(id);
		return;
	}
	d->obj = id;
	for (int k = 0; k < LI_N; k++)
		d->def[k] = -1;
	river_libinput_device_v1_add_listener(id, &input_li_listener, d);
	wl_list_insert(g_li_devices.prev, &d->link);
}

const struct river_libinput_config_v1_listener input_libinput_listener = {
	.finished = input_libinput_finished,
	.libinput_device = input_libinput_device,
};

bool input_bind_global(struct wl_registry *registry, u32 name,
		       const char *interface, u32 version)
{
	u32 v = (version < INPUT_VERSION_MAX) ? version : INPUT_VERSION_MAX;

	if (strcmp(interface, river_input_manager_v1_interface.name) == 0) {
		input_manager_v1 = wl_registry_bind(
			registry, name, &river_input_manager_v1_interface, v);
		if (input_manager_v1 != NULL)
			river_input_manager_v1_add_listener(
				input_manager_v1, &input_manager_listener,
				NULL);
		return true;
	}
	if (strcmp(interface, river_libinput_config_v1_interface.name) == 0) {
		input_libinput_v1 = wl_registry_bind(
			registry, name, &river_libinput_config_v1_interface, v);
		if (input_libinput_v1 != NULL)
			river_libinput_config_v1_add_listener(
				input_libinput_v1, &input_libinput_listener,
				NULL);
		return true;
	}
	return false;
}

void input_export_env(void)
{
	char size[16];

	if (g_config == NULL || g_config->input.cursor_theme == NULL)
		return;
	snprintf(size, sizeof(size), "%u",
		 (unsigned)g_config->input.cursor_size);
	setenv("XCURSOR_THEME", g_config->input.cursor_theme, 1);
	setenv("XCURSOR_SIZE", size, 1);
}

void input_apply_cursor(struct Seat *seat)
{
	if (g_config == NULL || g_config->input.cursor_theme == NULL ||
	    seat == NULL || seat->obj == NULL)
		return;
	river_seat_v1_set_xcursor_theme(seat->obj, g_config->input.cursor_theme,
					g_config->input.cursor_size);
}

void input_apply_all(void)
{
	struct LibinputDevice *ld;
	struct InputDevice *d;
	struct Seat *seat;

	wl_list_for_each(d, &g_devices, link)
		apply_repeat(d);
	wl_list_for_each(ld, &g_li_devices, link)
		apply_touchpad(ld);
	wl_list_for_each(seat, &wm.seats, link)
		input_apply_cursor(seat);
	input_export_env();
	keymap_apply();
}

void input_shutdown(void)
{
	struct LibinputDevice *ld, *ldtmp;
	struct InputDevice *d, *dtmp;

	wl_list_for_each_safe(d, dtmp, &g_devices, link) {
		river_input_device_v1_destroy(d->obj);
		wl_list_remove(&d->link);
		free(d);
	}
	wl_list_for_each_safe(ld, ldtmp, &g_li_devices, link) {
		river_libinput_device_v1_destroy(ld->obj);
		wl_list_remove(&ld->link);
		free(ld);
	}
}
