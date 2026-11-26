/* SPDX-License-Identifier: 0BSD */
/*
 * Status broadcast IPC. See status.h for the API contract and
 * docs/status-stream.md for the wire format.
 *
 * Implementation outline:
 *  - status_init opens an AF_UNIX server socket at the path
 *    described in the header. Non-blocking, FD_CLOEXEC, listen
 *    backlog 4 (we expect a handful of subscribers).
 *  - A fixed-size subscriber array tracks at most
 *    NM_STATUS_MAX_SUBS clients. accept(2) returns into it with
 *    O_NONBLOCK + FD_CLOEXEC set; drops on EAGAIN-on-write or
 *    POLLERR / POLLHUP.
 *  - status_publish walks wm.outputs / wm.workspaces / each
 *    workspace's window list once per call, emits a logfmt frame
 *    into a growing scratch buffer, FNV-1a hashes it, compares to
 *    the last hash, and writes the frame to every subscriber.
 *    Same buffer is cached as the replay-on-connect bytes.
 *  - The growing scratch buffer is owned by g_status; capacity
 *    doubles on demand. Steady-state allocation-free once cap
 *    stabilises.
 *
 * Failure modes:
 *  - bind / listen failure: log to stderr, status_init returns
 *    -1, every subsequent call is a no-op.
 *  - subscriber write EAGAIN / EPIPE: the subscriber is closed
 *    and the slot zeroed; the WM cycle keeps going.
 *  - snapshot build malloc failure: skip publish, no broadcast,
 *    no cache update; next cycle tries again.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client-core.h>

#include "config.h"
#include "layout.h"
#include "nomonad.h"
#include "status.h"

#define NM_STATUS_MAX_SUBS 16
#define NM_STATUS_FALLBACK_DIR "/tmp"
#define NM_STATUS_INITIAL_CAP 1024

struct StatusSub {
	int fd; /* -1 if slot unused */
};

struct StatusBuf {
	char *bytes;
	size_t len;
	size_t cap;
};

struct StatusPrivacy {
	bool emit_titles;
	bool emit_appid;
	bool emit_pid;
	bool debug;
};

struct StatusState {
	int server_fd;
	char *socket_path; /* owned; NULL if init failed */
	struct StatusSub subs[NM_STATUS_MAX_SUBS];
	struct StatusBuf scratch;
	struct StatusBuf last_msg; /* replayed to new subscribers */
	u64 last_hash;
	u64 seq;
	u32 next_synth_id;
	struct StatusPrivacy privacy;
};

static struct StatusState *g_status;

/* ---- helpers --------------------------------------------------------- */

static bool env_flag_default(const char *name, bool def)
{
	const char *v;

	v = getenv(name);
	if (v == NULL || *v == '\0')
		return def;
	if (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' ||
	    v[0] == 'F')
		return false;
	if (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' ||
	    v[0] == 'T')
		return true;
	return def;
}

/*
 * FNV-1a 64-bit. Cheap, decent distribution; the worst false-equal
 * rate at our snapshot sizes (<= a few KB) is well below the hash-
 * collision floor that would make us actually skip a real change.
 */
static u64 fnv1a_64(const char *bytes, size_t len)
{
	const u64 prime = 0x100000001b3ull;
	u64 hash;
	size_t i;

	hash = 0xcbf29ce484222325ull;
	for (i = 0; i < len; i++) {
		hash ^= (u64)(unsigned char)bytes[i];
		hash *= prime;
	}
	return hash;
}

static int buf_grow(struct StatusBuf *buf, size_t need)
{
	size_t newcap;
	char *grown;

	if (buf->cap >= need)
		return 0;
	newcap = buf->cap ? buf->cap : NM_STATUS_INITIAL_CAP;
	while (newcap < need) {
		if (newcap > (SIZE_MAX / 2))
			return -1;
		newcap *= 2;
	}
	grown = realloc(buf->bytes, newcap);
	if (grown == NULL)
		return -1;
	buf->bytes = grown;
	buf->cap = newcap;
	return 0;
}

static int buf_append(struct StatusBuf *buf, const char *src, size_t n)
{
	if (buf_grow(buf, buf->len + n + 1) < 0)
		return -1;
	memcpy(buf->bytes + buf->len, src, n);
	buf->len += n;
	buf->bytes[buf->len] = '\0';
	return 0;
}

static int buf_appendf(struct StatusBuf *buf, const char *fmt, ...)
{
	va_list ap, ap2;
	int n;
	size_t avail;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		va_end(ap2);
		return -1;
	}
	if (buf_grow(buf, buf->len + (size_t)n + 1) < 0) {
		va_end(ap2);
		return -1;
	}
	avail = buf->cap - buf->len;
	(void)vsnprintf(buf->bytes + buf->len, avail, fmt, ap2);
	va_end(ap2);
	buf->len += (size_t)n;
	return 0;
}

/*
 * Logfmt value emit. Empty strings, NULL pointers and values
 * containing ' ', '=', '"', '\\' or any control byte get wrapped
 * in double quotes with " and \ escaped. Everything else is
 * emitted bare.
 */
static int buf_append_value(struct StatusBuf *buf, const char *value)
{
	const char *p;
	bool need_quote;

	if (value == NULL || *value == '\0')
		return buf_append(buf, "\"\"", 2);
	need_quote = false;
	for (p = value; *p != '\0'; p++) {
		unsigned char c = (unsigned char)*p;

		if (c <= 0x20 || c == '=' || c == '"' || c == '\\' ||
		    c == 0x7f) {
			need_quote = true;
			break;
		}
	}
	if (!need_quote)
		return buf_append(buf, value, strlen(value));
	if (buf_append(buf, "\"", 1) < 0)
		return -1;
	for (p = value; *p != '\0'; p++) {
		unsigned char c = (unsigned char)*p;

		if (c == '"' || c == '\\') {
			if (buf_append(buf, "\\", 1) < 0)
				return -1;
			if (buf_append(buf, (const char *)&c, 1) < 0)
				return -1;
			continue;
		}
		if (c < 0x20 || c == 0x7f) {
			char esc[8];
			int n;

			n = snprintf(esc, sizeof(esc), "\\x%02x", c);
			if (n < 0 || buf_append(buf, esc, (size_t)n) < 0)
				return -1;
			continue;
		}
		if (buf_append(buf, (const char *)&c, 1) < 0)
			return -1;
	}
	return buf_append(buf, "\"", 1);
}

/* ---- subscriber array ------------------------------------------------ */

static int sub_alloc_slot(void)
{
	int i;

	for (i = 0; i < NM_STATUS_MAX_SUBS; i++) {
		if (g_status->subs[i].fd < 0)
			return i;
	}
	return -1;
}

static void sub_drop_slot(int i)
{
	if (i < 0 || i >= NM_STATUS_MAX_SUBS)
		return;
	if (g_status->subs[i].fd >= 0) {
		close(g_status->subs[i].fd);
		g_status->subs[i].fd = -1;
	}
}

static void sub_write_all(int slot, const char *bytes, size_t len)
{
	const char *p = bytes;
	size_t left = len;
	ssize_t w;
	int fd;

	fd = g_status->subs[slot].fd;
	if (fd < 0)
		return;
	while (left > 0) {
		w = write(fd, p, left);
		if (w > 0) {
			p += w;
			left -= (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		/*
		 * EAGAIN means the subscriber's recv buffer is full; we
		 * could buffer per-client and try again next cycle, but
		 * that is unbounded memory for a slow consumer. Drop and
		 * let them reconnect. EPIPE / ECONNRESET / anything else
		 * means dead. Same outcome.
		 */
		sub_drop_slot(slot);
		return;
	}
}

static void broadcast(const char *bytes, size_t len)
{
	int i;

	for (i = 0; i < NM_STATUS_MAX_SUBS; i++) {
		if (g_status->subs[i].fd < 0)
			continue;
		sub_write_all(i, bytes, len);
	}
}

static void accept_clients(void)
{
	int fd;
	int slot;
	int flags;

	for (;;) {
		fd = accept(g_status->server_fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (errno == EINTR)
				continue;
			return;
		}
		flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0)
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		flags = fcntl(fd, F_GETFD, 0);
		if (flags >= 0)
			fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
		slot = sub_alloc_slot();
		if (slot < 0) {
			/*
			 * NM_STATUS_MAX_SUBS reached. Polite refusal: send
			 * one logfmt-shaped record explaining and close.
			 */
			static const char busy[] =
				"type=error reason=too_many_subscribers\n";
			(void)write(fd, busy, sizeof(busy) - 1);
			close(fd);
			continue;
		}
		g_status->subs[slot].fd = fd;
		if (g_status->last_msg.len > 0)
			sub_write_all(slot, g_status->last_msg.bytes,
				      g_status->last_msg.len);
	}
}

/* ---- snapshot builder ------------------------------------------------ */

static const char *layout_name(enum LayoutKind kind)
{
	size_t i;

	for (i = 0; i < layout_descriptors_n; i++) {
		if (layout_descriptors[i].kind == kind)
			return layout_descriptors[i].name;
	}
	return "unknown";
}

/*
 * river_window_v1.identifier is the stable id when present (v4+).
 * Pre-v4 servers, harness-injected windows, and any future case
 * where identifier never arrives fall back to a per-WM monotonic
 * counter encoded as "syn-<n>" so the namespace cannot collide
 * with real identifiers (server-issued ones are typed strings).
 * Synthetic ids use Window.serial, assigned once at creation.
 */
static void window_id_emit(struct StatusBuf *buf, struct Window *w)
{
	char synth[32];

	if (w->identifier != NULL && w->identifier[0] != '\0') {
		(void)buf_append_value(buf, w->identifier);
		return;
	}
	(void)snprintf(synth, sizeof(synth), "syn-%lu",
		       (unsigned long)w->serial);
	(void)buf_append(buf, synth, strlen(synth));
}

/*
 * Builds the body (everything between frame_start and frame_end)
 * into buf, leaving seq out so the caller can hash buf->bytes
 * independently of the monotonic counter. publish_status wraps
 * the output with frame_start / frame_end markers only after the
 * dedupe gate succeeds, so quiet sessions do not bump seq just
 * because the body happened to be rebuilt.
 */
static int snapshot_build_body(struct StatusBuf *buf)
{
	struct Output *o;
	int oi;
	size_t i;

	buf->len = 0;

	oi = 0;
	wl_list_for_each(o, &wm.outputs, link) {
		if (buf_appendf(buf,
				"type=output index=%d visible_workspace=%d\n",
				oi, o->visible_workspace) < 0)
			return -1;
		oi++;
	}

	for (i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws;
		struct Window *focused;
		struct Output *show;
		struct Seat *s;

		ws = &wm.workspaces[i];
		show = output_showing((int)i);
		focused = NULL;
		wl_list_for_each(s, &wm.seats, link) {
			if (s->output != NULL && s->output == show &&
			    s->focused != NULL &&
			    s->focused->workspace_index == (int)i) {
				focused = s->focused;
				break;
			}
		}
		if (buf_appendf(buf, "type=workspace index=%zu", i) < 0)
			return -1;
		if (buf_append(buf, " focused_window=", 16) < 0)
			return -1;
		if (focused != NULL)
			window_id_emit(buf, focused);
		else
			buf_append(buf, "-", 1);
		if (buf_append(buf, " layout=", 8) < 0)
			return -1;
		if (buf_append_value(buf,
				     layout_name(workspace_layout(ws)->kind)) < 0)
			return -1;
		if (buf_append(buf, "\n", 1) < 0)
			return -1;
	}

	for (i = 0; i < wm.n_workspaces; i++) {
		struct Workspace *ws;
		struct Window *w;
		struct Output *show;
		struct Seat *s;

		ws = &wm.workspaces[i];
		show = output_showing((int)i);
		wl_list_for_each(w, &ws->windows, workspace_link) {
			bool focused;

			if (w->closed || w->requested_close)
				continue;
			focused = false;
			wl_list_for_each(s, &wm.seats, link) {
				if (s->output != NULL && s->output == show &&
				    s->focused == w) {
					focused = true;
					break;
				}
			}
			if (buf_append(buf, "type=window id=", 15) < 0)
				return -1;
			window_id_emit(buf, w);
			if (buf_appendf(buf, " workspace=%zu", i) < 0)
				return -1;
			if (g_status->privacy.emit_appid) {
				if (buf_append(buf, " app_id=", 8) < 0)
					return -1;
				if (buf_append_value(buf,
						     w->app_id ? w->app_id :
								 "") < 0)
					return -1;
			}
			if (g_status->privacy.emit_titles) {
				if (buf_append(buf, " title=", 7) < 0)
					return -1;
				if (buf_append_value(buf,
						     w->title ? w->title :
								"") < 0)
					return -1;
			}
			if (buf_appendf(buf,
					" focused=%d floating=%d fullscreen=%d",
					focused ? 1 : 0,
					w->floating ? 1 : 0,
					w->fullscreen ? 1 : 0) < 0)
				return -1;
			if (g_status->privacy.emit_pid &&
			    w->unreliable_pid > 0) {
				if (buf_appendf(buf, " pid=%d",
						w->unreliable_pid) < 0)
					return -1;
			}
			if (buf_append(buf, "\n", 1) < 0)
				return -1;
		}
	}

	return 0;
}

/* ---- public API ------------------------------------------------------ */

static char *socket_path_alloc(void)
{
	const char *runtime;
	char *path;
	int n;

	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime != NULL && *runtime != '\0') {
		n = (int)strlen(runtime) + 32;
		path = malloc((size_t)n);
		if (path == NULL)
			return NULL;
		snprintf(path, (size_t)n, "%s/nomonad-status.sock",
			 runtime);
		return path;
	}
	n = 64;
	path = malloc((size_t)n);
	if (path == NULL)
		return NULL;
	snprintf(path, (size_t)n,
		 NM_STATUS_FALLBACK_DIR "/nomonad-status-%u.sock",
		 (unsigned)getuid());
	return path;
}

static bool status_socket_live(const struct sockaddr_un *addr)
{
	int fd;
	int r;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	r = connect(fd, (const struct sockaddr *)addr, sizeof(*addr));
	close(fd);
	return r == 0;
}

int status_init(void)
{
	struct sockaddr_un addr;
	struct StatusState *s;
	int fd;
	int i;
	int flags;

	if (g_status != NULL)
		return 0;
	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return -1;
	for (i = 0; i < NM_STATUS_MAX_SUBS; i++)
		s->subs[i].fd = -1;
	s->server_fd = -1;
	s->privacy.emit_titles = env_flag_default("NM_STATUS_TITLES", false);
	s->privacy.emit_appid = env_flag_default("NM_STATUS_APPID", true);
	s->privacy.emit_pid = env_flag_default("NM_STATUS_PID", false);
	s->privacy.debug = env_flag_default("NM_STATUS_DEBUG", false);

	s->socket_path = socket_path_alloc();
	if (s->socket_path == NULL) {
		free(s);
		return -1;
	}
	if (strlen(s->socket_path) >= sizeof(addr.sun_path)) {
		fprintf(stderr,
			"nomonad: status socket path too long: %s\n",
			s->socket_path);
		free(s->socket_path);
		free(s);
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "nomonad: status socket failed: %s\n",
			strerror(errno));
		free(s->socket_path);
		free(s);
		return -1;
	}
	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, s->socket_path, strlen(s->socket_path) + 1);
	if (status_socket_live(&addr)) {
		fprintf(stderr,
			"nomonad: %s is owned by a running instance, "
			"status broadcast disabled\n",
			s->socket_path);
		close(fd);
		free(s->socket_path);
		free(s);
		return -1;
	}
	(void)unlink(s->socket_path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "nomonad: status bind %s failed: %s\n",
			s->socket_path, strerror(errno));
		close(fd);
		free(s->socket_path);
		free(s);
		return -1;
	}
	if (listen(fd, 4) < 0) {
		fprintf(stderr, "nomonad: status listen failed: %s\n",
			strerror(errno));
		close(fd);
		(void)unlink(s->socket_path);
		free(s->socket_path);
		free(s);
		return -1;
	}
	s->server_fd = fd;
	g_status = s;
	fprintf(stderr, "nomonad: status broadcast on %s\n",
		s->socket_path);
	return 0;
}

void status_shutdown(void)
{
	int i;

	if (g_status == NULL)
		return;
	for (i = 0; i < NM_STATUS_MAX_SUBS; i++)
		sub_drop_slot(i);
	if (g_status->server_fd >= 0) {
		close(g_status->server_fd);
		g_status->server_fd = -1;
	}
	if (g_status->socket_path != NULL) {
		(void)unlink(g_status->socket_path);
		free(g_status->socket_path);
	}
	free(g_status->scratch.bytes);
	free(g_status->last_msg.bytes);
	free(g_status);
	g_status = NULL;
}

size_t status_fill_pollfds(struct pollfd *out, size_t cap)
{
	size_t n;
	int i;

	if (g_status == NULL || out == NULL || cap == 0)
		return 0;
	n = 0;
	if (g_status->server_fd >= 0 && n < cap) {
		out[n].fd = g_status->server_fd;
		out[n].events = POLLIN;
		out[n].revents = 0;
		n++;
	}
	for (i = 0; i < NM_STATUS_MAX_SUBS && n < cap; i++) {
		if (g_status->subs[i].fd < 0)
			continue;
		out[n].fd = g_status->subs[i].fd;
		/*
		 * No POLLIN: subscribers are read-only from our side.
		 * We poll anyway so POLLERR / POLLHUP / POLLNVAL surface
		 * promptly and we drop the slot before a stale fd gets
		 * matched against an unrelated reuse.
		 */
		out[n].events = 0;
		out[n].revents = 0;
		n++;
	}
	return n;
}

void status_handle_events(struct pollfd *fds, size_t n)
{
	bool accept_pending = false;
	size_t k;
	int i;

	if (g_status == NULL || fds == NULL)
		return;
	for (k = 0; k < n; k++) {
		if (fds[k].fd == g_status->server_fd) {
			if (fds[k].revents & POLLIN)
				accept_pending = true;
			continue;
		}
		if (!(fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)))
			continue;
		for (i = 0; i < NM_STATUS_MAX_SUBS; i++) {
			if (g_status->subs[i].fd == fds[k].fd) {
				sub_drop_slot(i);
				break;
			}
		}
	}
	if (accept_pending)
		accept_clients();
}

void status_publish(bool status_dirty, u64 start_unix_ns,
		    u64 end_unix_ns)
{
	struct StatusBuf *scratch;
	struct StatusBuf *out;
	u64 hash;

	if (g_status == NULL || !status_dirty)
		return;

	scratch = &g_status->scratch;
	if (snapshot_build_body(scratch) < 0)
		return;

	hash = fnv1a_64(scratch->bytes, scratch->len);
	if (g_status->last_hash == hash && g_status->last_msg.len > 0) {
		if (g_status->privacy.debug)
			fprintf(stderr,
				"nomonad: status publish skipped (hash hit)\n");
		return;
	}
	g_status->last_hash = hash;
	g_status->seq++;

	/*
	 * Assemble the broadcast bytes into last_msg with the seq-
	 * carrying frame markers wrapping the deduped body. last_msg
	 * doubles as the replay-on-connect bytes for new subscribers.
	 * Done in last_msg directly (not via swap) so the body in
	 * scratch can be reused on the next publish without reset.
	 *
	 * Timestamps live on the markers, not in the body, so the body
	 * hash stays stable across cycles where only time advanced.
	 * Emitted as wall-clock unix_ns from CLOCK_REALTIME (sampled
	 * by the caller in wm_handle_manage_start so the bracket
	 * covers the actual manage work, not this assembly step).
	 */
	out = &g_status->last_msg;
	out->len = 0;
	if (buf_appendf(out,
			"type=frame_start seq=%llu time_start_unix_ns=%llu\n",
			(unsigned long long)g_status->seq,
			(unsigned long long)start_unix_ns) < 0)
		return;
	if (buf_append(out, scratch->bytes, scratch->len) < 0)
		return;
	if (buf_appendf(out,
			"type=frame_end seq=%llu time_end_unix_ns=%llu\n",
			(unsigned long long)g_status->seq,
			(unsigned long long)end_unix_ns) < 0)
		return;

	broadcast(out->bytes, out->len);
	if (g_status->privacy.debug) {
		int n_subs;
		int i;

		n_subs = 0;
		for (i = 0; i < NM_STATUS_MAX_SUBS; i++)
			if (g_status->subs[i].fd >= 0)
				n_subs++;
		fprintf(stderr,
			"nomonad: status publish %zu bytes -> %d subs (seq %llu)\n",
			out->len, n_subs,
			(unsigned long long)g_status->seq);
	}
}
