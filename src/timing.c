/* SPDX-License-Identifier: 0BSD */
/*
 * Per-cycle wall-clock instrumentation. See timing.h for the API and
 * docs/session-format.md style for the on-disk line shape (timing
 * shares the "T <ns> ..." lead-in with the session-trace recorder so
 * one parser can handle both).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "nomonad.h"
#include "recorder.h"
#include "timing.h"

#define NM_TIMING_MAX_BYTES (64ULL * 1024 * 1024)

struct Timing {
	FILE *out;
	struct timespec origin;
	struct timespec t_manage_begin;
	struct timespec t_render_begin;
	u64 pending_manage_ns;
	u64 written;
	bool manage_open;
	bool pending_dirty;
	bool stopped;
};

static struct Timing *g_timing;

static long long ts_delta_ns(const struct timespec *a, const struct timespec *b)
{
	return (long long)(a->tv_sec - b->tv_sec) * 1000000000LL +
	       (long long)(a->tv_nsec - b->tv_nsec);
}

static void timing_make_parent(const char *path)
{
	const char *slash;
	char buf[4096];
	size_t i;

	slash = strrchr(path, '/');
	if (slash == NULL || slash == path)
		return;
	if ((size_t)(slash - path) >= sizeof(buf))
		return;
	memcpy(buf, path, (size_t)(slash - path));
	buf[slash - path] = '\0';
	for (i = 1; buf[i] != '\0'; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0700) != 0 && errno != EEXIST)
			return;
		buf[i] = '/';
	}
	mkdir(buf, 0700);
}

static size_t timing_count_windows(void)
{
	size_t total;
	size_t i;

	total = 0;
	for (i = 0; i < wm.n_workspaces; i++) {
		struct Window *w;

		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link)
			total++;
	}
	return total;
}

int timing_init(void)
{
	const char *path = getenv("NM_TIMING");
	struct Timing *t;

	if (path == NULL || *path == '\0')
		return 0;
	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return -1;
	timing_make_parent(path);
	t->out = recorder_open_private(path);
	if (t->out == NULL) {
		fprintf(stderr,
			"nomonad: NM_TIMING='%s': %s; timing disabled\n",
			path, strerror(errno));
		free(t);
		return 0;
	}
	/*
	 * Line buffered, not block buffered. The earlier block-buffered
	 * design lost the in-flight buffer on SIGTERM (river closing
	 * killed nomonad before atexit could fire), leaving partial
	 * trailing data that did not match a complete line. Per-event
	 * write(2) syscall is ~200 ns; well below the 8 us p50 manage
	 * cycle this measures, and matches the recorder's posture.
	 */
	setvbuf(t->out, NULL, _IOLBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &t->origin);
	g_timing = t;
	fprintf(stderr, "nomonad: writing timing to %s\n", path);
	atexit(timing_shutdown);
	return 0;
}

void timing_shutdown(void)
{
	if (g_timing == NULL)
		return;
	fflush(g_timing->out);
	fclose(g_timing->out);
	free(g_timing);
	g_timing = NULL;
}

void timing_begin_manage(void)
{
	if (g_timing == NULL)
		return;
	clock_gettime(CLOCK_MONOTONIC, &g_timing->t_manage_begin);
	g_timing->manage_open = true;
}

void timing_end_manage(void)
{
	struct timespec now;

	if (g_timing == NULL || !g_timing->manage_open)
		return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	g_timing->pending_manage_ns =
		(u64)ts_delta_ns(&now, &g_timing->t_manage_begin);
	g_timing->manage_open = false;
}

void timing_begin_render(void)
{
	if (g_timing == NULL)
		return;
	clock_gettime(CLOCK_MONOTONIC, &g_timing->t_render_begin);
}

void timing_observe_dirty(bool dirty)
{
	if (g_timing == NULL)
		return;
	g_timing->pending_dirty = dirty;
}

void timing_end_render(void)
{
	struct timespec now;
	long long delta_ns;
	u64 render_ns;
	size_t windows;
	int n;

	if (g_timing == NULL)
		return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	render_ns = (u64)ts_delta_ns(&now, &g_timing->t_render_begin);
	delta_ns = ts_delta_ns(&now, &g_timing->origin);
	windows = timing_count_windows();
	if (g_timing->stopped)
		return;
	if (g_timing->written >= NM_TIMING_MAX_BYTES) {
		g_timing->stopped = true;
		fprintf(stderr,
			"nomonad: timing log hit %llu byte cap; "
			"timing stopped\n",
			(unsigned long long)NM_TIMING_MAX_BYTES);
		return;
	}
	n = fprintf(
		g_timing->out,
		"T %lld manage_us=%llu render_us=%llu windows=%zu dirty=%d\n",
		delta_ns,
		(unsigned long long)(g_timing->pending_manage_ns / 1000),
		(unsigned long long)(render_ns / 1000), windows,
		g_timing->pending_dirty ? 1 : 0);
	if (n > 0)
		g_timing->written += (u64)n;
	g_timing->pending_manage_ns = 0;
	g_timing->pending_dirty = false;
}
