/* SPDX-License-Identifier: 0BSD */
/*
 * Out-of-band spawn helper. Forked once at nomonad startup before any
 * libwayland / xkbcommon init so the helper inherits a small parent
 * (cheap fork). Subsequent ACTION_SPAWN dispatch sends the command
 * over a pipe instead of fork+exec'ing inline; the WM cycle never
 * pays for fork/exec syscalls again.
 *
 * Live timing of a real session showed every manage cycle over
 * 200 us was spawn-triggered before this existed.
 *
 * Lifecycle:
 *   spawnd_init        startup, before wl_display_connect
 *   spawnd_send_command per ACTION_SPAWN dispatch
 *   spawnd_shutdown     called via atexit; helper exits on EOF
 *
 * The helper is structurally fail-safe: if init or any subsequent
 * write fails, the caller (seat.c spawn_child) falls back to inline
 * fork+exec, restoring the prior behavior. Spawn is never silently
 * dropped without a stderr line.
 */

#ifndef NOMONAD_SPAWND_H
#define NOMONAD_SPAWND_H

int spawnd_init(void);
int spawnd_send_command(const char *cmd);
void spawnd_shutdown(void);

/*
 * Set DISPLAY in the calling process to a plausible Xwayland
 * socket if it is not already set. Scans /tmp/.X11-unix for
 * X<n> entries, picks the newest by mtime, exports DISPLAY=:<n>.
 * No-op when DISPLAY is already set, when the directory is
 * absent, or when no X<n> sockets are found. Call from inside
 * the post-fork pre-exec child of a spawn so X11 apps launched
 * from a key bind find Xwayland without a wrapper script.
 *
 * Idempotent across xwayland restarts when called per-spawn:
 * each grandchild gets a fresh detection. Returns 0 on success
 * (DISPLAY now set, was already set, or no sockets found - all
 * legitimate "you can exec now" outcomes), -1 only on resource
 * failure that would likely affect the exec too.
 */
int spawnd_apply_xwayland_env(void);
void spawnd_child_setup(void);
void spawnd_exec(const char *cmd) __attribute__((noreturn));

#endif /* NOMONAD_SPAWND_H */
