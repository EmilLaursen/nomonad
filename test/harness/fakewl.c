/* SPDX-License-Identifier: 0BSD */
/*
 * Linker-wrapped stubs for the libwayland-client proxy entry points.
 * Each generated river_*_v1_* request is a static inline that calls
 * wl_proxy_marshal_flags / get_user_data / get_version / add_listener
 * / destroy. Wrapping those at link time (-Wl,--wrap=...) lets the WM
 * modules link unchanged into a test binary without ever opening a
 * wayland connection.
 *
 * A "fake proxy" is the small allocation a wrapped marshal_flags hands
 * back when the request would have minted a new wayland object. It only
 * stores user_data and the protocol version; nothing in the WM ever
 * looks past those. Layout must match harness.c's mirror struct.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client-core.h>

#include "fakewl.h"

struct PhaseRule {
	const char *iface;
	uint32_t op;
	int phases;
	const char *name;
};

static const struct PhaseRule phase_rules[] = {
#include "phase_table.inc"
};

enum FakewlPhase fakewl_phase;
static struct fake_proxy *g_dead;

static void proxy_bury(struct fake_proxy *p)
{
	if (p->dead) {
		fprintf(stderr, "fakewl: %s destroyed twice\n",
			p->iface != NULL ? p->iface : "proxy");
		abort();
	}
	p->dead = 1;
	p->next_dead = g_dead;
	g_dead = p;
}

void fakewl_reap(void)
{
	struct fake_proxy *next;

	while (g_dead != NULL) {
		next = g_dead->next_dead;
		free(g_dead);
		g_dead = next;
	}
}

static void phase_check(const struct fake_proxy *p, uint32_t op)
{
	if (p == NULL)
		return;
	if (p->dead) {
		fprintf(stderr, "fakewl: request %u on destroyed %s\n", op,
			p->iface != NULL ? p->iface : "proxy");
		abort();
	}
	if (p->iface == NULL || fakewl_phase == FAKEWL_TEARDOWN)
		return;
	for (size_t i = 0; i < sizeof(phase_rules) / sizeof(phase_rules[0]);
	     i++) {
		if (phase_rules[i].op != op ||
		    strcmp(phase_rules[i].iface, p->iface) != 0)
			continue;
		if ((phase_rules[i].phases & (int)fakewl_phase) == 0) {
			fprintf(stderr, "fakewl: %s.%s outside its sequence\n",
				p->iface, phase_rules[i].name);
			abort();
		}
	}
}

/*
 * Wire-message counter. Every wl_proxy_marshal_flags call corresponds
 * to exactly one libwayland-client request (one wire-protocol message
 * to the server) regardless of whether it mints a new proxy or not.
 * The harness uses this as the perf metric the README calls out:
 * request churn per manage / render cycle. Forward-declared in
 * harness_lib.h; defined here so the counter sits next to the only
 * code that bumps it.
 */
static uint64_t g_request_count;
static uint32_t g_last_opcode;
static const char *g_track_name;
static struct wl_proxy *g_tracked[32];
static size_t g_n_tracked;

uint32_t harness_last_opcode(void)
{
	return g_last_opcode;
}

void harness_track_iface(const char *name)
{
	g_track_name = name;
	g_n_tracked = 0;
}

size_t harness_tracked_count(void)
{
	return g_n_tracked;
}

struct wl_proxy *harness_tracked(size_t i)
{
	return (i < g_n_tracked) ? g_tracked[i] : NULL;
}

uint64_t harness_request_count_get(void)
{
	return g_request_count;
}

void harness_request_count_reset(void)
{
	g_request_count = 0;
}

struct wl_proxy *
__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
			      const struct wl_interface *interface,
			      uint32_t version, uint32_t flags, ...)
{
	struct fake_proxy *result;

	g_last_opcode = opcode;
	g_request_count++;
	phase_check((struct fake_proxy *)proxy, opcode);
	result = NULL;
	if (interface != NULL) {
		result = calloc(1, sizeof(*result));
		if (result != NULL) {
			result->version = version;
			result->iface = interface->name;
		}
		if (result != NULL && g_track_name != NULL &&
		    strcmp(interface->name, g_track_name) == 0 &&
		    g_n_tracked < sizeof(g_tracked) / sizeof(g_tracked[0]))
			g_tracked[g_n_tracked++] = (struct wl_proxy *)result;
	}
	if ((flags & WL_MARSHAL_FLAG_DESTROY) && proxy != NULL)
		proxy_bury((struct fake_proxy *)proxy);
	return (struct wl_proxy *)result;
}

void __wrap_wl_proxy_destroy(struct wl_proxy *proxy)
{
	if (proxy != NULL)
		proxy_bury((struct fake_proxy *)proxy);
}

void *__wrap_wl_proxy_get_user_data(struct wl_proxy *proxy)
{
	if (proxy == NULL)
		return NULL;
	return ((struct fake_proxy *)proxy)->user_data;
}

void __wrap_wl_proxy_set_user_data(struct wl_proxy *proxy, void *data)
{
	if (proxy == NULL)
		return;
	((struct fake_proxy *)proxy)->user_data = data;
}

uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy)
{
	if (proxy == NULL)
		return 0;
	return ((struct fake_proxy *)proxy)->version;
}

int __wrap_wl_proxy_add_listener(struct wl_proxy *proxy,
				 void (**implementation)(void), void *data)
{
	if (proxy == NULL)
		return -1;
	((struct fake_proxy *)proxy)->user_data = data;
	((struct fake_proxy *)proxy)->implementation = implementation;
	return 0;
}
