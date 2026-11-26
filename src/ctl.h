/* SPDX-License-Identifier: 0BSD */
#ifndef NOMONAD_CTL_H
#define NOMONAD_CTL_H

#include <poll.h>
#include <stddef.h>

int ctl_init(void (*wake)(void));
void ctl_shutdown(void);
size_t ctl_fill_pollfds(struct pollfd *out, size_t cap);
void ctl_handle_events(struct pollfd *fds, size_t n);
void ctl_dispatch(void);
int ctl_client(const char *command);

#endif /* NOMONAD_CTL_H */
