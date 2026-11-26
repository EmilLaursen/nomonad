/* SPDX-License-Identifier: 0BSD */
/*
 * Long-lived spawn helper. See spawnd.h for the API contract.
 *
 * Wire format on the pipe (host byte order; single-process pair):
 *   u32  cmd_len   (1..NM_SPAWND_MAX_CMD)
 *   char      cmd[cmd_len]
 *
 * spawnd_send_command issues both as one writev(2). Total message
 * size stays under PIPE_BUF (4096 on Linux), so the write is atomic;
 * the helper is guaranteed to read full message frames in order even
 * if multiple senders ever existed.
 *
 * Failure modes:
 *   - pipe2/fork at init: log + return -1; caller falls back inline.
 *   - writev EPIPE / EBADF: helper died; close fd, return -1.
 *   - protocol violation in helper (oversized cmd_len): exit cleanly,
 *     parent will see EPIPE on next send and degrade to inline.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "spawnd.h"
#include "types.h"

#define NM_SPAWND_MAX_CMD 1024
#define NM_XDIR "/tmp/.X11-unix"

static int g_pipe_fd = -1;

/*
 * Find the most recently modified Xwayland socket and write its
 * display name (":<n>", e.g. ":1") into out. Returns 0 on success,
 * -1 if no socket was found or the directory is unreadable.
 *
 * Heuristic: pick newest mtime. If the user has a real X server
 * also running (Xvfb in a CI container, an old session still
 * alive), the newer of the two wins; close enough for the
 * "launch xterm from a bind" use case. Callers that want to
 * override should set DISPLAY explicitly in the spawn command.
 */
static int find_xwayland_display(char *out, size_t cap)
{
	struct dirent *de;
	struct stat st;
	char path[64];
	DIR *dir;
	time_t newest_mtime;
	int newest_num;
	int num;

	if (out == NULL || cap < 4) /* ":N\0" needs >=3 */
		return -1;
	dir = opendir(NM_XDIR);
	if (dir == NULL)
		return -1;
	newest_num = -1;
	newest_mtime = 0;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] != 'X')
			continue;
		if (sscanf(de->d_name + 1, "%d", &num) != 1)
			continue;
		if (num < 0)
			continue;
		if (snprintf(path, sizeof(path), NM_XDIR "/%s",
			     de->d_name) >= (int)sizeof(path))
			continue;
		if (stat(path, &st) < 0)
			continue;
		/*
		 * S_ISSOCK filters stale junk in /tmp/.X11-unix that
		 * happens to start with X. The dir is normally root-
		 * owned and only sockets land here, but be defensive.
		 */
		if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid())
			continue;
		if (st.st_mtime > newest_mtime || newest_num < 0) {
			newest_mtime = st.st_mtime;
			newest_num = num;
		}
	}
	closedir(dir);
	if (newest_num < 0)
		return -1;
	if (snprintf(out, cap, ":%d", newest_num) >= (int)cap)
		return -1;
	return 0;
}

int spawnd_apply_xwayland_env(void)
{
	char display[16];

	if (getenv("DISPLAY") != NULL && *getenv("DISPLAY") != '\0')
		return 0;
	if (find_xwayland_display(display, sizeof(display)) < 0)
		return 0; /* no sockets is a legitimate "nothing to do" */
	(void)setenv("DISPLAY", display, 0);
	return 0;
}

static bool read_full(int fd, void *buf, size_t n)
{
	char *p = buf;
	ssize_t r;

	while (n > 0) {
		r = read(fd, p, n);
		if (r > 0) {
			p += r;
			n -= (size_t)r;
			continue;
		}
		if (r == -1 && errno == EINTR)
			continue;
		return false;
	}
	return true;
}

/*
 * Helper-side: fork + setsid + redirect stdin to /dev/null + exec
 * /bin/sh -c cmd. Mirrors the inline fallback in seat.c spawn_child.
 * Comments on signal hygiene and the SIGCHLD = SIG_IGN trap live
 * there; the contents are intentionally identical.
 */
void spawnd_child_setup(void)
{
	struct sigaction sa = { 0 };
	sigset_t empty;
	int devnull;
	int sig;

	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	for (sig = 1; sig <= SIGRTMAX; sig++) {
		if (sig == SIGKILL || sig == SIGSTOP)
			continue;
		sigaction(sig, &sa, NULL);
	}
	sigemptyset(&empty);
	sigprocmask(SIG_SETMASK, &empty, NULL);
	setsid();

	devnull = open("/dev/null", O_RDONLY);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		if (devnull != STDIN_FILENO)
			close(devnull);
	}

	/*
	 * Per-spawn xwayland detection so X11 apps launched from a
	 * bind find the current Xwayland socket. Done in the grand-
	 * child (post-fork, pre-exec) so the helper's own DISPLAY
	 * stays unset and each spawn picks up the live state -
	 * matters when xwayland restarts and the X<n> number changes
	 * mid-session.
	 */
	(void)spawnd_apply_xwayland_env();
}

static bool scope_available(void)
{
	const char *env = getenv("NM_SPAWN_SCOPE");
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[512];
	struct stat st;

	if (env != NULL && strcmp(env, "0") == 0)
		return false;
	if (rt == NULL || rt[0] != '/')
		return false;
	snprintf(path, sizeof(path), "%s/systemd/private", rt);
	return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static void scope_unit(const char *cmd, char *out, size_t cap)
{
	struct timespec ts;
	const char *word = cmd;
	const char *end;
	const char *base;
	size_t n;
	int len;

	for (;;) {
		while (*word == ' ' || *word == '\t')
			word++;
		end = word;
		while (*end != '\0' && *end != ' ' && *end != '\t')
			end++;
		if (end == word ||
		    memchr(word, '=', (size_t)(end - word)) == NULL)
			break;
		word = end;
	}
	base = word;
	for (const char *p = word; p < end; p++)
		if (*p == '/')
			base = p + 1;
	len = snprintf(out, cap, "--unit=app-nomonad-");
	n = (len > 0) ? (size_t)len : 0;
	for (const char *p = base; p < end && n + 1 < cap && n < 48; p++) {
		char ch = *p;

		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '_' || ch == '.'))
			ch = '_';
		out[n++] = ch;
	}
	if (base == end && n + 4 < cap) {
		memcpy(out + n, "cmd", 3);
		n += 3;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	snprintf(out + n, cap - n, "-%ld-%lld%09ld", (long)getpid(),
		 (long long)ts.tv_sec, ts.tv_nsec);
}

void spawnd_exec(const char *cmd)
{
	char unit[160];

	if (scope_available()) {
		scope_unit(cmd, unit, sizeof(unit));
		execlp("systemd-run", "systemd-run", "--user", "--scope",
		       "--collect", "--quiet", "--slice=app.slice", unit, "--",
		       "/bin/sh", "-c", cmd, (char *)NULL);
	}
	execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
	_exit(127);
}

static void helper_spawn_one(const char *cmd)
{
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return;
	if (pid != 0)
		return;

	spawnd_child_setup();
	spawnd_exec(cmd);
}

static void __attribute__((noreturn)) helper_main(int read_fd)
{
	char cmd[NM_SPAWND_MAX_CMD + 1];
	u32 len;

	/*
	 * SIG_IGN on SIGCHLD so the kernel auto-reaps grandchildren the
	 * helper forks; the helper itself never waits on them. SIGPIPE
	 * is irrelevant on the read side but ignored defensively.
	 */
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	prctl(PR_SET_NAME, "nm-spawnd", 0, 0, 0);
	unsetenv("WAYLAND_DEBUG");

	for (;;) {
		if (!read_full(read_fd, &len, sizeof(len)))
			break;
		if (len == 0 || len > NM_SPAWND_MAX_CMD)
			break;
		if (!read_full(read_fd, cmd, len))
			break;
		cmd[len] = '\0';
		helper_spawn_one(cmd);
	}
	_exit(0);
}

int spawnd_init(void)
{
	int pipefd[2];
	pid_t pid;
	int fl;

	if (pipe(pipefd) < 0) {
		fprintf(stderr,
			"nomonad: spawnd pipe failed: %s; spawn falls back inline\n",
			strerror(errno));
		return -1;
	}
	for (int i = 0; i < 2; i++) {
		fl = fcntl(pipefd[i], F_GETFD, 0);
		if (fl != -1)
			fcntl(pipefd[i], F_SETFD, fl | FD_CLOEXEC);
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr,
			"nomonad: spawnd fork failed: %s; spawn falls back inline\n",
			strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pipefd[1]);
		helper_main(pipefd[0]);
	}

	close(pipefd[0]);
	fl = fcntl(pipefd[1], F_GETFL, 0);
	if (fl != -1)
		fcntl(pipefd[1], F_SETFL, fl | O_NONBLOCK);
	g_pipe_fd = pipefd[1];
	return 0;
}

int spawnd_send_command(const char *cmd)
{
	struct iovec iov[2];
	size_t cmd_len;
	u32 len;
	ssize_t wrote;

	if (g_pipe_fd < 0 || cmd == NULL)
		return -1;
	cmd_len = strlen(cmd);
	if (cmd_len == 0)
		return -1;
	if (cmd_len > NM_SPAWND_MAX_CMD) {
		fprintf(stderr,
			"nomonad: spawn cmd too long (%zu bytes, max %d); skipping\n",
			cmd_len, NM_SPAWND_MAX_CMD);
		return -1;
	}
	len = (u32)cmd_len;

	iov[0].iov_base = &len;
	iov[0].iov_len = sizeof(len);
	iov[1].iov_base = (void *)cmd;
	iov[1].iov_len = cmd_len;

	wrote = writev(g_pipe_fd, iov, 2);
	if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		fprintf(stderr,
			"nomonad: spawnd busy; spawn falls back inline\n");
		return -1;
	}
	if (wrote < 0) {
		fprintf(stderr,
			"nomonad: spawnd write failed: %s; spawn falls back inline\n",
			strerror(errno));
		close(g_pipe_fd);
		g_pipe_fd = -1;
		return -1;
	}
	return 0;
}

void spawnd_shutdown(void)
{
	if (g_pipe_fd >= 0) {
		close(g_pipe_fd);
		g_pipe_fd = -1;
	}
}
