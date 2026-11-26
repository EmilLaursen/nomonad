/* SPDX-License-Identifier: 0BSD */
#ifndef NOMONAD_RESTORE_H
#define NOMONAD_RESTORE_H

#include <stdbool.h>

void restore_set_argv(char **argv);
void restore_request(void);
bool restore_take_request(void);
const char *restore_state_path(void);
void restore_exec(void);
int restore_save(const char *path);
int restore_load(const char *path);
void restore_apply(void);

#endif /* NOMONAD_RESTORE_H */
