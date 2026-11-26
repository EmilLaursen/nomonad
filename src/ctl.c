/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client-core.h>

#include "config.h"
#include "ctl.h"
#include "nomonad.h"

#define NM_CTL_MAX_CLIENTS 8
#define NM_CTL_LINE_MAX 4096

struct CtlClient {
	char in[NM_CTL_LINE_MAX];
	size_t in_len;
	char *pending;
	bool eof;
	int fd;
};

struct CtlState {
	struct CtlClient clients[NM_CTL_MAX_CLIENTS];
	void (*wake)(void);
	char *path;
	int server_fd;
};

static struct CtlState *g_ctl;

static char *ctl_path_alloc(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char *path;
	size_t n;

	if (runtime != NULL && runtime[0] != '\0') {
		n = strlen(runtime) + 32;
		path = malloc(n);
		if (path != NULL)
			snprintf(path, n, "%s/nomonad-ctl.sock", runtime);
		return path;
	}
	n = 64;
	path = malloc(n);
	if (path != NULL)
		snprintf(path, n, "/tmp/nomonad-ctl-%u.sock",
			 (unsigned)getuid());
	return path;
}

static int ctl_addr(const char *path, struct sockaddr_un *addr)
{
	if (strlen(path) >= sizeof(addr->sun_path))
		return -1;
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	memcpy(addr->sun_path, path, strlen(path) + 1);
	return 0;
}

static void set_flags(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static bool socket_live(const struct sockaddr_un *addr)
{
	int fd;
	int r;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	r = connect(fd, (const struct sockaddr *)addr, sizeof(*addr));
	close(fd);
	return r == 0;
}

int ctl_init(void (*wake)(void))
{
	struct sockaddr_un addr;
	struct CtlState *s;
	int fd;

	if (g_ctl != NULL)
		return 0;
	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return -1;
	for (int i = 0; i < NM_CTL_MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
	s->wake = wake;
	s->server_fd = -1;
	s->path = ctl_path_alloc();
	if (s->path == NULL || ctl_addr(s->path, &addr) < 0)
		goto fail;
	if (socket_live(&addr)) {
		fprintf(stderr, "nomonad: %s is owned by a running instance\n",
			s->path);
		goto fail;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		goto fail;
	set_flags(fd);
	(void)unlink(s->path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 4) < 0) {
		fprintf(stderr, "nomonad: ctl socket %s: %s\n", s->path,
			strerror(errno));
		close(fd);
		goto fail;
	}
	s->server_fd = fd;
	g_ctl = s;
	fprintf(stderr, "nomonad: commands on %s\n", s->path);
	return 0;
fail:
	free(s->path);
	free(s);
	return -1;
}

static void client_drop(struct CtlClient *c)
{
	if (c->fd >= 0)
		close(c->fd);
	free(c->pending);
	c->pending = NULL;
	c->in_len = 0;
	c->eof = false;
	c->fd = -1;
}

void ctl_shutdown(void)
{
	if (g_ctl == NULL)
		return;
	for (int i = 0; i < NM_CTL_MAX_CLIENTS; i++)
		client_drop(&g_ctl->clients[i]);
	if (g_ctl->server_fd >= 0) {
		close(g_ctl->server_fd);
		(void)unlink(g_ctl->path);
	}
	free(g_ctl->path);
	free(g_ctl);
	g_ctl = NULL;
}

static void client_send(struct CtlClient *c, const char *s)
{
	size_t len = strlen(s);
	ssize_t n;

	if (c->fd < 0)
		return;
	n = send(c->fd, s, len, MSG_NOSIGNAL);
	if (n < 0 || (size_t)n != len)
		client_drop(c);
}

static void client_reply(struct CtlClient *c, const char *err)
{
	char buf[512];

	if (err == NULL) {
		client_send(c, "ok\n");
		return;
	}
	snprintf(buf, sizeof(buf), "error %s\n", err);
	client_send(c, buf);
}

static void clean_field(char *dst, size_t cap, const char *src)
{
	size_t n = 0;

	if (src == NULL)
		src = "";
	for (; *src != '\0' && n + 1 < cap; src++)
		dst[n++] = (*src == '\t' || *src == '\n' || *src == '\r') ?
				   ' ' :
				   *src;
	dst[n] = '\0';
}

static void window_id(const struct Window *w, char *buf, size_t cap)
{
	if (w->identifier != NULL && w->identifier[0] != '\0')
		snprintf(buf, cap, "%s", w->identifier);
	else
		snprintf(buf, cap, "syn-%lu", (unsigned long)w->serial);
}

static struct Seat *first_seat(void)
{
	struct Seat *seat;

	if (wl_list_empty(&wm.seats))
		return NULL;
	return wl_container_of(wm.seats.next, seat, link);
}

static void cmd_list(struct CtlClient *c)
{
	char title[256], app[128], id[64], ws[24];
	char line[512];
	struct Seat *seat;
	struct Window *w;

	seat = first_seat();
	for (size_t i = 0; i < wm.n_workspaces && c->fd >= 0; i++) {
		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			if (w->closed || w->ignored)
				continue;
			window_id(w, id, sizeof(id));
			if ((int)i == wm_scratch_ws_index())
				snprintf(ws, sizeof(ws), "-");
			else
				snprintf(ws, sizeof(ws), "%zu", i + 1);
			clean_field(app, sizeof(app), w->app_id);
			clean_field(title, sizeof(title), w->title);
			snprintf(line, sizeof(line), "%s\t%s\t%d\t%d\t%s\t%s\n",
				 id, ws, seat != NULL && seat->focused == w,
				 (int)w->unreliable_pid, app, title);
			client_send(c, line);
			if (c->fd < 0)
				return;
		}
	}
	client_reply(c, NULL);
}

static struct Window *find_window(const char *id)
{
	char buf[64];
	struct Window *w;

	for (size_t i = 0; i < wm.n_workspaces; i++) {
		wl_list_for_each(w, &wm.workspaces[i].windows, workspace_link) {
			if (w->closed)
				continue;
			window_id(w, buf, sizeof(buf));
			if (strcmp(buf, id) == 0)
				return w;
		}
	}
	return NULL;
}

static const char *cmd_window(const char *line, struct Seat *seat)
{
	const char *id;
	struct Window *w;
	bool close;

	close = strncmp(line, "close-window ", 13) == 0;
	id = line + 13;
	while (*id == ' ')
		id++;
	w = find_window(id);
	if (w == NULL)
		return "no such window";
	if (close)
		return seat_close_window(seat, w) ? NULL : "window is closing";
	return seat_focus_window(seat, w) ? NULL : "window cannot be focused";
}

static void cmd_run(struct CtlClient *c, const char *line)
{
	struct Action action;
	struct Seat *seat;
	char err[256];

	seat = first_seat();
	if (seat == NULL) {
		client_reply(c, "no seat");
		return;
	}
	if (strncmp(line, "focus-window ", 13) == 0 ||
	    strncmp(line, "close-window ", 13) == 0) {
		client_reply(c, cmd_window(line, seat));
		return;
	}
	if (config_parse_action_text(g_config, line, &action, err,
				     sizeof(err)) < 0) {
		client_reply(c, err);
		return;
	}
	if (action.kind == ACTION_MOVE || action.kind == ACTION_RESIZE ||
	    action.kind == ACTION_SUBMAP) {
		free(action.arg);
		client_reply(c, "action needs a key or pointer");
		return;
	}
	seat_run_action(seat, &action);
	free(action.arg);
	client_reply(c, NULL);
}

static char *find_nl(char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] == '\n')
			return p + i;
	return NULL;
}

static void client_take_lines(struct CtlClient *c)
{
	char *nl;
	size_t len;

	while (c->fd >= 0 && c->pending == NULL &&
	       (nl = find_nl(c->in, c->in_len)) != NULL) {
		*nl = '\0';
		len = (size_t)(nl - c->in) + 1;
		if (nl > c->in && nl[-1] == '\r')
			nl[-1] = '\0';
		if (strcmp(c->in, "list") == 0) {
			cmd_list(c);
		} else if (c->in[0] != '\0') {
			c->pending = strdup(c->in);
			if (c->pending == NULL)
				client_reply(c, "out of memory");
			else if (g_ctl->wake != NULL)
				g_ctl->wake();
		}
		if (c->fd < 0)
			return;
		memmove(c->in, c->in + len, c->in_len - len);
		c->in_len -= len;
	}
	if (c->fd >= 0 && c->pending == NULL && c->eof)
		client_drop(c);
}

static void client_read(struct CtlClient *c)
{
	ssize_t n;

	for (;;) {
		if (c->in_len == sizeof(c->in)) {
			if (find_nl(c->in, c->in_len) != NULL)
				break;
			client_reply(c, "line too long");
			client_drop(c);
			return;
		}
		n = read(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len);
		if (n > 0) {
			c->in_len += (size_t)n;
			continue;
		}
		if (n == 0) {
			if (c->in_len > 0 &&
			    find_nl(c->in, c->in_len) == NULL) {
				c->in[c->in_len++] = '\n';
			}
			c->eof = true;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			client_drop(c);
			return;
		}
		break;
	}
	client_take_lines(c);
}

static void accept_clients(void)
{
	struct ucred cred;
	socklen_t len;
	int slot;
	int fd;

	for (;;) {
		fd = accept(g_ctl->server_fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		set_flags(fd);
		len = sizeof(cred);
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
		    cred.uid != getuid()) {
			close(fd);
			continue;
		}
		slot = -1;
		for (int i = 0; i < NM_CTL_MAX_CLIENTS; i++) {
			if (g_ctl->clients[i].fd < 0) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			(void)send(fd, "error busy\n", 11, MSG_NOSIGNAL);
			close(fd);
			continue;
		}
		g_ctl->clients[slot].fd = fd;
	}
}

size_t ctl_fill_pollfds(struct pollfd *out, size_t cap)
{
	size_t n = 0;

	if (g_ctl == NULL || out == NULL)
		return 0;
	if (n < cap) {
		out[n].fd = g_ctl->server_fd;
		out[n].events = POLLIN;
		out[n].revents = 0;
		n++;
	}
	for (int i = 0; i < NM_CTL_MAX_CLIENTS && n < cap; i++) {
		if (g_ctl->clients[i].fd < 0 || g_ctl->clients[i].eof ||
		    g_ctl->clients[i].pending != NULL)
			continue;
		out[n].fd = g_ctl->clients[i].fd;
		out[n].events = POLLIN;
		out[n].revents = 0;
		n++;
	}
	return n;
}

void ctl_handle_events(struct pollfd *fds, size_t n)
{
	struct CtlClient *c;
	bool accept_pending = false;

	if (g_ctl == NULL || fds == NULL)
		return;
	for (size_t k = 0; k < n; k++) {
		if (fds[k].revents == 0)
			continue;
		if (fds[k].fd == g_ctl->server_fd) {
			accept_pending = true;
			continue;
		}
		for (int i = 0; i < NM_CTL_MAX_CLIENTS; i++) {
			c = &g_ctl->clients[i];
			if (c->fd == fds[k].fd) {
				client_read(c);
				break;
			}
		}
	}
	if (accept_pending)
		accept_clients();
}

void ctl_dispatch(void)
{
	struct CtlClient *c;
	char *line;

	if (g_ctl == NULL)
		return;
	for (int i = 0; i < NM_CTL_MAX_CLIENTS; i++) {
		c = &g_ctl->clients[i];
		if (c->fd < 0 || c->pending == NULL)
			continue;
		line = c->pending;
		c->pending = NULL;
		cmd_run(c, line);
		free(line);
		if (c->fd >= 0)
			client_take_lines(c);
	}
}

int ctl_client(const char *command)
{
	struct sockaddr_un addr;
	size_t cap = 4096;
	size_t len = 0;
	char *path;
	char *grown;
	char *last;
	char *buf;
	ssize_t n;
	int fd;

	path = ctl_path_alloc();
	if (path == NULL || ctl_addr(path, &addr) < 0) {
		free(path);
		return 2;
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "nomonad: cannot connect to %s: %s\n", path,
			strerror(errno));
		free(path);
		if (fd >= 0)
			close(fd);
		return 2;
	}
	free(path);
	if (send(fd, command, strlen(command), MSG_NOSIGNAL) < 0 ||
	    send(fd, "\n", 1, MSG_NOSIGNAL) < 0) {
		close(fd);
		return 2;
	}
	shutdown(fd, SHUT_WR);
	buf = malloc(cap);
	while (buf != NULL) {
		if (len + 1 == cap) {
			grown = realloc(buf, cap * 2);
			if (grown == NULL)
				break;
			buf = grown;
			cap *= 2;
		}
		n = read(fd, buf + len, cap - 1 - len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	if (buf == NULL)
		return 2;
	buf[len] = '\0';
	last = len > 0 ? buf + len - 1 : buf;
	if (last > buf && *last == '\n')
		*last-- = '\0';
	while (last > buf && last[-1] != '\n')
		last--;
	fwrite(buf, 1, (size_t)(last - buf), stdout);
	if (strcmp(last, "ok") == 0) {
		free(buf);
		return 0;
	}
	fprintf(stderr, "nomonad: %s\n", last[0] != '\0' ? last : "no reply");
	free(buf);
	return 1;
}
