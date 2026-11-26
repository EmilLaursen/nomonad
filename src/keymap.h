/* SPDX-License-Identifier: 0BSD */
#ifndef NOMONAD_KEYMAP_H
#define NOMONAD_KEYMAP_H

#include <stdbool.h>

#include "types.h"

struct wl_registry;
struct river_xkb_config_v1;

bool keymap_bind_global(struct wl_registry *registry, u32 name,
			const char *interface, u32 version);
void keymap_attach(struct river_xkb_config_v1 *obj);
void keymap_apply(void);
void keymap_shutdown(void);

#endif /* NOMONAD_KEYMAP_H */
