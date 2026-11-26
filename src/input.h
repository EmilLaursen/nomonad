/* SPDX-License-Identifier: 0BSD */
#ifndef NOMONAD_INPUT_H
#define NOMONAD_INPUT_H

#include <stdbool.h>

#include "nomonad.h"

struct wl_registry;
struct river_input_manager_v1;
struct river_libinput_config_v1;

extern struct river_input_manager_v1 *input_manager_v1;
extern struct river_libinput_config_v1 *input_libinput_v1;

bool input_bind_global(struct wl_registry *registry, u32 name,
		       const char *interface, u32 version);
void input_export_env(void);
void input_apply_cursor(struct Seat *seat);
void input_apply_all(void);
void input_shutdown(void);

#endif /* NOMONAD_INPUT_H */
