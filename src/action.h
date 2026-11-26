/* SPDX-License-Identifier: 0BSD */
/*
 * Action helpers shared between binding.c and main.c.
 *
 * action_dup deep-copies an Action (strdup's the arg).
 * action_clear frees arg and resets kind to ACTION_NONE.
 * action_kind_name returns a static string for the enumerator (ACTION_
 * prefix stripped). Used by the recorder to write symbolic kind names
 * into the trace; the replayer parses them back via the corresponding
 * lookup. Never returns NULL (unknown values fall through to "NONE").
 */
#ifndef NOMONAD_ACTION_H
#define NOMONAD_ACTION_H

#include "nomonad.h"

struct Action action_dup(const struct Action *src);
void action_clear(struct Action *a);
bool action_kind_repeats(enum ActionKind kind);
const char *action_kind_name(enum ActionKind kind);
/*
 * Reverse of action_kind_name. Returns true on success and writes the
 * enumerator into *out; returns false if name is not a known kind. The
 * replayer uses this to decode press lines from a session trace.
 */
bool action_kind_from_name(const char *name, enum ActionKind *out);

#endif /* NOMONAD_ACTION_H */
