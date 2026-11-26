/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>
#include <river-xkb-config-v1-client-protocol.h>

#include "config.h"
#include "keymap.h"
#include "nomonad.h"

#define KEYMAP_VERSION_MAX 3

struct Keyboard {
	struct river_xkb_keyboard_v1 *obj;
	struct wl_list link;
};

struct Names {
	char *v[5];
	bool set;
};

static struct wl_list g_keyboards = { &g_keyboards, &g_keyboards };
static struct river_xkb_keymap_v1 *g_pending;
static struct river_xkb_keymap_v1 *g_active;
static struct river_xkb_config_v1 *g_xkbc;
static struct xkb_context *g_ctx;
static struct Names g_submitted;
static struct Names g_applied;
static bool g_ever_applied;

static void names_clear(struct Names *n)
{
	for (int i = 0; i < 5; i++) {
		free(n->v[i]);
		n->v[i] = NULL;
	}
	n->set = false;
}

static void kb_set(struct Keyboard *kb)
{
	if (g_active != NULL)
		river_xkb_keyboard_v1_set_keymap(kb->obj, g_active);
}

static void kb_removed(void *data, struct river_xkb_keyboard_v1 *obj)
{
	struct Keyboard *kb = data;

	river_xkb_keyboard_v1_destroy(kb->obj);
	wl_list_remove(&kb->link);
	free(kb);
}

static void kb_input_device(void *data, struct river_xkb_keyboard_v1 *obj,
			    struct river_input_device_v1 *device)
{
}

static void kb_layout(void *data, struct river_xkb_keyboard_v1 *obj, u32 index,
		      const char *name)
{
}

static void kb_noop(void *data, struct river_xkb_keyboard_v1 *obj)
{
}

static const struct river_xkb_keyboard_v1_listener kb_listener = {
	.removed = kb_removed,
	.input_device = kb_input_device,
	.layout = kb_layout,
	.capslock_enabled = kb_noop,
	.capslock_disabled = kb_noop,
	.numlock_enabled = kb_noop,
	.numlock_disabled = kb_noop,
	.done = kb_noop,
	.scrolllock_enabled = kb_noop,
	.scrolllock_disabled = kb_noop,
};

static void keymap_success(void *data, struct river_xkb_keymap_v1 *obj)
{
	struct Keyboard *kb;

	if (obj != g_pending) {
		river_xkb_keymap_v1_destroy(obj);
		return;
	}
	if (g_active != NULL)
		river_xkb_keymap_v1_destroy(g_active);
	g_active = obj;
	g_pending = NULL;
	names_clear(&g_applied);
	g_applied = g_submitted;
	memset(&g_submitted, 0, sizeof(g_submitted));
	g_ever_applied = g_ever_applied || g_applied.set;
	wl_list_for_each(kb, &g_keyboards, link)
		kb_set(kb);
}

static void keymap_failure(void *data, struct river_xkb_keymap_v1 *obj,
			   const char *msg)
{
	fprintf(stderr, "nomonad: river rejected the keymap: %s\n",
		msg != NULL ? msg : "?");
	if (obj == g_pending) {
		g_pending = NULL;
		names_clear(&g_submitted);
	}
	river_xkb_keymap_v1_destroy(obj);
}

static const struct river_xkb_keymap_v1_listener keymap_listener = {
	.success = keymap_success,
	.failure = keymap_failure,
};

static void xkbc_finished(void *data, struct river_xkb_config_v1 *obj)
{
}

static void xkbc_keyboard(void *data, struct river_xkb_config_v1 *obj,
			  struct river_xkb_keyboard_v1 *id)
{
	struct Keyboard *kb = calloc(1, sizeof(*kb));

	if (kb == NULL) {
		river_xkb_keyboard_v1_destroy(id);
		return;
	}
	kb->obj = id;
	river_xkb_keyboard_v1_add_listener(id, &kb_listener, kb);
	wl_list_insert(g_keyboards.prev, &kb->link);
	kb_set(kb);
}

static const struct river_xkb_config_v1_listener xkbc_listener = {
	.finished = xkbc_finished,
	.xkb_keyboard = xkbc_keyboard,
};

void keymap_attach(struct river_xkb_config_v1 *obj)
{
	g_xkbc = obj;
	river_xkb_config_v1_add_listener(obj, &xkbc_listener, NULL);
	keymap_apply();
}

bool keymap_bind_global(struct wl_registry *registry, u32 name,
			const char *interface, u32 version)
{
	u32 v = version < KEYMAP_VERSION_MAX ? version : KEYMAP_VERSION_MAX;
	struct river_xkb_config_v1 *obj;

	if (strcmp(interface, river_xkb_config_v1_interface.name) != 0)
		return false;
	obj = wl_registry_bind(registry, name, &river_xkb_config_v1_interface,
			       v);
	if (obj != NULL)
		keymap_attach(obj);
	return true;
}

static bool same_str(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

static bool names_equal(const struct Names *n, const struct InputConfig *ic)
{
	if (ic->keymap_set != n->set)
		return false;
	for (int i = 0; i < 5; i++)
		if (!same_str(ic->xkb[i], n->v[i]))
			return false;
	return true;
}

static void names_copy(struct Names *n, const struct InputConfig *ic)
{
	names_clear(n);
	for (int i = 0; i < 5; i++)
		n->v[i] = ic->xkb[i] != NULL ? strdup(ic->xkb[i]) : NULL;
	n->set = ic->keymap_set;
}

static int keymap_fd(const struct InputConfig *ic)
{
	struct xkb_rule_names names = { 0 };
	struct xkb_keymap *km;
	char *text;
	size_t len;
	int fd;

	if (g_ctx == NULL)
		g_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (g_ctx == NULL)
		return -1;
	if (ic->keymap_set) {
		names.rules = ic->xkb[XKB_NAME_RULES];
		names.model = ic->xkb[XKB_NAME_MODEL];
		names.layout = ic->xkb[XKB_NAME_LAYOUT];
		names.variant = ic->xkb[XKB_NAME_VARIANT];
		names.options = ic->xkb[XKB_NAME_OPTIONS];
	}
	km = xkb_keymap_new_from_names(g_ctx, &names,
				       XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (km == NULL)
		return -1;
	text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	xkb_keymap_unref(km);
	if (text == NULL)
		return -1;
	len = strlen(text) + 1;
	fd = memfd_create("nomonad-keymap", MFD_CLOEXEC);
	if (fd >= 0 && write(fd, text, len) != (ssize_t)len) {
		close(fd);
		fd = -1;
	}
	free(text);
	return fd;
}

void keymap_apply(void)
{
	const struct InputConfig *ic;
	int fd;

	if (g_xkbc == NULL || g_config == NULL)
		return;
	ic = &g_config->input;
	if (!ic->keymap_set && !g_ever_applied)
		return;
	if (g_pending != NULL ? names_equal(&g_submitted, ic) :
				names_equal(&g_applied, ic))
		return;
	fd = keymap_fd(ic);
	if (fd < 0) {
		fprintf(stderr, "nomonad: cannot build the keymap\n");
		return;
	}
	if (g_pending != NULL)
		river_xkb_keymap_v1_destroy(g_pending);
	g_pending = river_xkb_config_v1_create_keymap(
		g_xkbc, fd, RIVER_XKB_CONFIG_V1_KEYMAP_FORMAT_TEXT_V1);
	close(fd);
	if (g_pending != NULL)
		river_xkb_keymap_v1_add_listener(g_pending, &keymap_listener,
						 NULL);
	names_copy(&g_submitted, ic);
}

void keymap_shutdown(void)
{
	struct Keyboard *kb, *tmp;

	wl_list_for_each_safe(kb, tmp, &g_keyboards, link) {
		river_xkb_keyboard_v1_destroy(kb->obj);
		wl_list_remove(&kb->link);
		free(kb);
	}
	if (g_pending != NULL)
		river_xkb_keymap_v1_destroy(g_pending);
	if (g_active != NULL)
		river_xkb_keymap_v1_destroy(g_active);
	g_pending = NULL;
	g_active = NULL;
	g_xkbc = NULL;
	names_clear(&g_submitted);
	names_clear(&g_applied);
	g_ever_applied = false;
}
