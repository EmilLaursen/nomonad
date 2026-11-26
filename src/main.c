/* SPDX-License-Identifier: 0BSD */
/*
 * Derived from tinyrwm.c, (c) 2026 Isaac Freund, 0BSD.
 *
 * Entry point and out-of-band glue: the wl_registry binding, the
 * config-load + hot-reload subsystem (SIGUSR1, inotify), and the main
 * poll loop. The window-management state machinery lives in
 * output.c / window.c / seat.c / wm.c; this file just wires them up
 * and dispatches the event loop.
 */

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>

#include "binding.h"
#include "config.h"
#include "ctl.h"
#include "input.h"
#include "keymap.h"
#include "layout.h"
#include "nomonad.h"
#include "recorder.h"
#include "restore.h"
#include "spawnd.h"
#include "status.h"
#include "timing.h"

struct WindowManager wm;
struct river_window_manager_v1 *window_manager_v1;
struct river_xkb_bindings_v1 *xkb_bindings_v1;
struct river_layer_shell_v1 *layer_shell_v1;
struct Config *g_config;

/* Hot reload state. g_config_path is owned. */
static char *g_config_path;
static int g_sig_pipe[2] = { -1, -1 };
static int g_inotify_fd = -1;
static struct {
	int wd;
	char *base;
} g_watch[2];
static int g_n_watch;
volatile sig_atomic_t g_reload_pending;

/* ---- Registry --------------------------------------------------------- */

static void handle_global(void *data, struct wl_registry *registry, u32 name,
			  const char *interface, u32 version)
{
	if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
		if (version >= 4) {
			window_manager_v1 = wl_registry_bind(
				registry, name,
				&river_window_manager_v1_interface,
				(version < 5) ? version : 5);
		}
	} else if (strcmp(interface, river_xkb_bindings_v1_interface.name) ==
		   0) {
		if (version >= 2)
			xkb_bindings_v1 = wl_registry_bind(
				registry, name,
				&river_xkb_bindings_v1_interface,
				(version < 3) ? version : 3);
	} else if (strcmp(interface, river_layer_shell_v1_interface.name) ==
		   0) {
		layer_shell_v1 = wl_registry_bind(
			registry, name, &river_layer_shell_v1_interface, 1);
	} else if (!keymap_bind_global(registry, name, interface, version)) {
		input_bind_global(registry, name, interface, version);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
				 u32 name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

/* ---- config + reload -------------------------------------------------- */

static struct Config *config_load_usable(const char *path, char *err,
					 size_t errlen)
{
	struct Config *cfg;

	cfg = config_load(path, err, errlen);
	if (cfg != NULL && cfg->n_xkb == 0) {
		snprintf(err, errlen, "%s: no key bindings (defkeys), refusing",
			 path);
		config_free(cfg);
		cfg = NULL;
	}
	return cfg;
}

static char g_config_notice[512];
static bool g_config_notice_delay;

static void config_notice_send(void)
{
	static const char head[] =
		"notify-send -a nomonad -u critical 'nomonad config' '";
	static const char wait[] = "sleep 5; ";
	char cmd[sizeof(wait) + sizeof(head) + 4 * sizeof(g_config_notice) + 2];
	size_t n = 0;

	if (g_config_notice[0] == '\0')
		return;
	if (g_config_notice_delay) {
		memcpy(cmd, wait, sizeof(wait) - 1);
		n = sizeof(wait) - 1;
	}
	memcpy(cmd + n, head, sizeof(head) - 1);
	n += sizeof(head) - 1;
	for (const char *p = g_config_notice; *p != '\0'; p++) {
		if (*p == '\'') {
			memcpy(cmd + n, "'\\''", 4);
			n += 4;
		} else {
			cmd[n++] = (*p >= ' ' && *p <= '~') ? *p : ' ';
		}
	}
	cmd[n++] = '\'';
	cmd[n] = '\0';
	if (spawnd_send_command(cmd) == 0)
		g_config_notice[0] = '\0';
	g_config_notice_delay = false;
}

static void config_init(const char *override_path)
{
	const char *home;
	const char *xdg;
	char path[PATH_MAX];

	if (override_path != NULL && *override_path != '\0') {
		g_config_path = strdup(override_path);
	} else {
		path[0] = '\0';
		xdg = getenv("XDG_CONFIG_HOME");
		home = getenv("HOME");
		if (xdg != NULL && *xdg != '\0')
			snprintf(path, sizeof(path), "%s/nomonad/config.scm",
				 xdg);
		else if (home != NULL && *home != '\0')
			snprintf(path, sizeof(path),
				 "%s/.config/nomonad/config.scm", home);
		if (path[0] != '\0')
			g_config_path = strdup(path);
	}

	if (g_config_path != NULL && access(g_config_path, R_OK) == 0) {
		char err[256];

		g_config = config_load_usable(g_config_path, err, sizeof(err));
		if (g_config == NULL) {
			fprintf(stderr, "nomonad: %s\n", err);
			fprintf(stderr,
				"nomonad: falling back to default config\n");
			snprintf(g_config_notice, sizeof(g_config_notice),
				 "%s, using the default config", err);
			g_config_notice_delay = true;
		}
	} else if (override_path != NULL && *override_path != '\0') {
		fprintf(stderr, "nomonad: cannot read config '%s': %s\n",
			override_path, strerror(errno));
		fprintf(stderr, "nomonad: falling back to default config\n");
	}
	if (g_config == NULL)
		g_config = config_default();
}

static void sigusr_handler(int sig)
{
	if (g_sig_pipe[1] >= 0) {
		ssize_t n;
		char b;

		b = sig == SIGUSR2 ? 'R' : 'r';
		n = write(g_sig_pipe[1], &b, 1);
		(void)n;
	}
}

static void setup_signals(void)
{
	struct sigaction sa = { 0 };

	if (pipe(g_sig_pipe) < 0)
		return;
	for (int i = 0; i < 2; i++) {
		int fl, fd;

		fl = fcntl(g_sig_pipe[i], F_GETFL, 0);
		if (fl != -1)
			fcntl(g_sig_pipe[i], F_SETFL, fl | O_NONBLOCK);
		fd = fcntl(g_sig_pipe[i], F_GETFD, 0);
		if (fd != -1)
			fcntl(g_sig_pipe[i], F_SETFD, fd | FD_CLOEXEC);
	}

	sa.sa_handler = sigusr_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
}

static void watch_add(const char *path)
{
	const char *dirname;
	char *slash;
	char *copy;
	int wd;

	if (g_n_watch >= (int)ARRAY_SIZE(g_watch))
		return;
	copy = strdup(path);
	if (copy == NULL)
		return;
	slash = strrchr(copy, '/');
	if (slash == NULL) {
		dirname = ".";
		slash = copy - 1;
	} else if (slash == copy) {
		dirname = "/";
	} else {
		*slash = '\0';
		dirname = copy;
	}
	wd = inotify_add_watch(g_inotify_fd, dirname,
			       IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (wd >= 0) {
		g_watch[g_n_watch].wd = wd;
		g_watch[g_n_watch].base = strdup(slash + 1);
		if (g_watch[g_n_watch].base != NULL)
			g_n_watch++;
	}
	free(copy);
}

static void setup_inotify(void)
{
	char *real;

	if (g_config_path == NULL)
		return;
	g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (g_inotify_fd < 0)
		return;
	watch_add(g_config_path);
	real = realpath(g_config_path, NULL);
	if (real != NULL && strcmp(real, g_config_path) != 0)
		watch_add(real);
	free(real);
}

/*
 * Read a file fully into a malloc'd buffer, NUL-terminated. Returns
 * NULL on any failure. Used by the recorder to capture the on-disk
 * config text at init and on reload; the config loader does not
 * preserve the source string. Caller frees.
 */
static char *config_slurp(const char *path)
{
	long n;
	FILE *f;
	char *buf;
	size_t got;

	if (path == NULL)
		return NULL;
	f = fopen(path, "r");
	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	n = ftell(f);
	if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	got = fread(buf, 1, (size_t)n, f);
	buf[got] = '\0';
	fclose(f);
	return buf;
}

/*
 * SIGUSR1 and inotify both call this. Mark the reload pending and ask
 * the server for a manage cycle; reload_now() runs at the start of
 * wm_handle_manage_start. Reloading here directly would touch xkb
 * binding state outside a manage sequence and trigger river's
 * "invalid modification of window management state" error.
 */
void schedule_reload(void)
{
	g_reload_pending = 1;
	river_window_manager_v1_manage_dirty(window_manager_v1);
}

static void ctl_wake(void)
{
	river_window_manager_v1_manage_dirty(window_manager_v1);
}

static void drain_sig_pipe(void)
{
	bool restart = false;
	bool reload = false;
	char buf[64];
	ssize_t n;

	while ((n = read(g_sig_pipe[0], buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == 'R')
				restart = true;
			else
				reload = true;
		}
	}
	if (restart) {
		restore_request();
		river_window_manager_v1_manage_dirty(window_manager_v1);
	}
	if (reload)
		schedule_reload();
}

static void drain_inotify(void)
{
	bool dirty = false;
	char buf[4096]
		__attribute__((aligned(__alignof__(struct inotify_event))));
	for (;;) {
		ssize_t r = read(g_inotify_fd, buf, sizeof(buf));
		if (r <= 0)
			break;
		for (char *p = buf; p < buf + r;) {
			struct inotify_event *e = (struct inotify_event *)p;

			if (e->mask & IN_Q_OVERFLOW)
				dirty = true;
			for (int i = 0; e->len > 0 && i < g_n_watch; i++) {
				if (e->wd == g_watch[i].wd &&
				    strcmp(e->name, g_watch[i].base) == 0)
					dirty = true;
			}
			p += sizeof(*e) + e->len;
		}
	}
	if (dirty)
		schedule_reload();
}

void reload_now(void)
{
	struct Config *staging;
	char *reloaded_text;
	char err[256];

	if (g_config_path == NULL)
		return;
	staging = config_load_usable(g_config_path, err, sizeof(err));
	if (staging == NULL) {
		fprintf(stderr, "nomonad: reload failed: %s\n", err);
		snprintf(g_config_notice, sizeof(g_config_notice),
			 "%s, keeping the previous config", err);
		config_notice_send();
		return;
	}
	fprintf(stderr, "nomonad: reloaded config from %s\n", g_config_path);
	reloaded_text = config_slurp(g_config_path);
	recorder_event_config_reload(reloaded_text);
	free(reloaded_text);
	wm_swap_config(staging);
}

/* ---- main ------------------------------------------------------------- */

/*
 * Pollset budget: 1 wayland + up to 2 control (sig / inotify) + N
 * seat repeat timers + 1 status server + up to NM_STATUS_MAX_SUBS
 * (16) status subscribers. Round up so a multi-seat session stays
 * well clear of the cap.
 */
#define NM_MAX_POLL_FDS 64

#ifndef NM_VERSION
#define NM_VERSION "git"
#endif

static void usage(FILE *out)
{
	fputs("Usage: nomonad [OPTION]...\n"
	      "\n"
	      "Options:\n"
	      "  -c, --config PATH   load config from PATH instead of\n"
	      "                      $XDG_CONFIG_HOME/nomonad/config.scm\n"
	      "  -m, --msg COMMAND   send COMMAND to the running nomonad\n"
	      "                      and print the reply. COMMAND is list\n"
	      "                      (id, workspace, focused, pid, app_id,\n"
	      "                      title, tab separated),\n"
	      "                      focus-window ID, close-window ID or\n"
	      "                      any config action, e.g. '(workspace 3)'\n"
	      "      --describe      print the config schema as s-expressions\n"
	      "  -h, --help          show this help and exit\n"
	      "  -V, --version       print version and exit\n",
	      out);
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "msg", required_argument, NULL, 'm' },
		{ "describe", no_argument, NULL, 'D' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ 0, 0, 0, 0 },
	};
	struct wl_registry *registry;
	struct wl_display *display;
	const char *config_arg = NULL;
	char *restore_file = NULL;
	const char *env;
	int wayland_fd;
	int opt;

	while ((opt = getopt_long(argc, argv, "c:m:hV", long_opts, NULL)) !=
	       -1) {
		switch (opt) {
		case 'c':
			config_arg = optarg;
			break;
		case 'm':
			return ctl_client(optarg);
		case 'D':
			config_describe(stdout);
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		case 'V':
			fputs("nomonad " NM_VERSION "\n", stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "nomonad: unexpected argument '%s'\n",
			argv[optind]);
		usage(stderr);
		return 2;
	}

	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	env = getenv("NOMONAD_RESTORE");
	if (env != NULL) {
		restore_file = strdup(env);
		unsetenv("NOMONAD_RESTORE");
	}
	restore_set_argv(argv);

	signal(SIGPIPE, SIG_IGN);
	config_init(config_arg);
	input_export_env();

	/*
	 * Fork the spawn helper before any heavyweight init (libwayland,
	 * xkbcommon) so the helper inherits a small parent and its fork
	 * cost stays cheap regardless of how much state the WM later
	 * accumulates. Failure here is non-fatal: spawn_child falls back
	 * to inline fork+exec, restoring the prior behavior.
	 */
	if (spawnd_init() == 0)
		atexit(spawnd_shutdown);
	config_notice_send();

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to connect to Wayland server\n");
		return 1;
	}

	/*
	 * Avoid passing WAYLAND_DEBUG on to our children. It only matters
	 * if it is set when the display is created.
	 */
	unsetenv("WAYLAND_DEBUG");

	signal(SIGCHLD, SIG_IGN);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "roundtrip failed\n");
		return 1;
	}

	if (window_manager_v1 == NULL || xkb_bindings_v1 == NULL) {
		fprintf(stderr,
			"river_window_manager_v1 or river_xkb_bindings_v1 "
			"not supported by the Wayland server\n");
		return 1;
	}

	setup_signals();
	setup_inotify();
	wm_init();
	if (restore_file != NULL) {
		if (restore_load(restore_file) != 0)
			fprintf(stderr,
				"nomonad: cannot read restore state %s\n",
				restore_file);
		free(restore_file);
	}

	if (recorder_init() == 0) {
		char *initial_text = config_slurp(g_config_path);

		recorder_event_config_set(initial_text);
		free(initial_text);
	}

	timing_init();
	if (status_init() == 0)
		atexit(status_shutdown);
	if (ctl_init(ctl_wake) == 0)
		atexit(ctl_shutdown);

	river_window_manager_v1_add_listener(window_manager_v1, &wm_listener,
					     NULL);

	wayland_fd = wl_display_get_fd(display);

	while (true) {
		struct pollfd pfds[NM_MAX_POLL_FDS];
		struct Seat *seat;
		nfds_t sig_idx;
		nfds_t ino_idx;
		nfds_t wl_idx;
		nfds_t status_start;
		nfds_t ctl_start;
		size_t status_n;
		size_t ctl_n;
		nfds_t nfds;
		int r;

		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);

		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(display);
			fprintf(stderr, "flush failed\n");
			return 1;
		}

		/*
		 * pfds: 1 wayland + up to 2 control (sig / inotify) + N
		 * seat repeat timers + 1 status server + up to
		 * NM_STATUS_MAX_SUBS (16) subscribers. NM_MAX_POLL_FDS
		 * bounds the lot. The status block is appended last so
		 * status_handle_events can index into pfds via
		 * status_start.
		 */
		nfds = 0;
		pfds[nfds].fd = wayland_fd;
		pfds[nfds].events = POLLIN;
		wl_idx = nfds++;
		sig_idx = (nfds_t)-1;
		ino_idx = (nfds_t)-1;
		if (g_sig_pipe[0] >= 0) {
			pfds[nfds].fd = g_sig_pipe[0];
			pfds[nfds].events = POLLIN;
			sig_idx = nfds++;
		}
		if (g_inotify_fd >= 0) {
			pfds[nfds].fd = g_inotify_fd;
			pfds[nfds].events = POLLIN;
			ino_idx = nfds++;
		}
		wl_list_for_each(seat, &wm.seats, link) {
			if (nfds >= NM_MAX_POLL_FDS)
				break;
			if (seat->repeat_timer_fd < 0)
				continue;
			pfds[nfds].fd = seat->repeat_timer_fd;
			pfds[nfds].events = POLLIN;
			nfds++;
		}
		status_start = nfds;
		status_n = status_fill_pollfds(pfds + nfds,
					       NM_MAX_POLL_FDS - nfds);
		nfds += (nfds_t)status_n;
		ctl_start = nfds;
		ctl_n = ctl_fill_pollfds(pfds + nfds, NM_MAX_POLL_FDS - nfds);
		nfds += (nfds_t)ctl_n;

		r = poll(pfds, nfds, -1);
		if (r < 0) {
			wl_display_cancel_read(display);
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			return 1;
		}

		if (pfds[wl_idx].revents & POLLIN) {
			if (wl_display_read_events(display) < 0) {
				fprintf(stderr, "read_events failed\n");
				return 1;
			}
		} else {
			wl_display_cancel_read(display);
		}
		if (wl_display_dispatch_pending(display) < 0) {
			fprintf(stderr, "dispatch_pending failed\n");
			return 1;
		}

		if (sig_idx != (nfds_t)-1 && (pfds[sig_idx].revents & POLLIN))
			drain_sig_pipe();
		if (ino_idx != (nfds_t)-1 && (pfds[ino_idx].revents & POLLIN))
			drain_inotify();
		if (status_n > 0)
			status_handle_events(pfds + status_start, status_n);
		if (ctl_n > 0)
			ctl_handle_events(pfds + ctl_start, ctl_n);
		/*
		 * Drain every seat's repeat timer non-blockingly. Cheap on
		 * idle (returns EAGAIN). On expiration, sets pending_action
		 * and calls manage_dirty so the next manage cycle fires it.
		 */
		wl_list_for_each(seat, &wm.seats, link)
			drain_repeat_timer(seat);
		/*
		 * Reload itself happens in wm_handle_manage_start (must be
		 * inside a manage sequence). drain_sig_pipe / drain_inotify
		 * have already called manage_dirty so the next iteration
		 * picks it up.
		 */
	}

	return 0;
}
