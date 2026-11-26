/* SPDX-License-Identifier: 0BSD */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "action.h"

struct Action action_dup(const struct Action *src)
{
	struct Action a = { .kind = src->kind, .arg = NULL };
	if (src->arg != NULL)
		a.arg = strdup(src->arg);
	return a;
}

void action_clear(struct Action *a)
{
	free(a->arg);
	a->arg = NULL;
	a->kind = ACTION_NONE;
}

bool action_kind_repeats(enum ActionKind kind)
{
	switch (kind) {
	case ACTION_FOCUS_NEXT:
	case ACTION_FOCUS_PREV:
	case ACTION_SWAP_NEXT:
	case ACTION_SWAP_PREV:
	case ACTION_SHRINK_MASTER:
	case ACTION_EXPAND_MASTER:
	case ACTION_INC_MASTER:
	case ACTION_DEC_MASTER:
	case ACTION_RESIZE_DELTA:
	case ACTION_NEXT_WORKSPACE:
	case ACTION_PREV_WORKSPACE:
	case ACTION_LAYOUT_VERB:
	case ACTION_NEXT_LAYOUT:
	case ACTION_PREV_LAYOUT:
		return true;
	default:
		return false;
	}
}

const char *action_kind_name(enum ActionKind kind)
{
	switch (kind) {
	case ACTION_NONE:
		return "NONE";
	case ACTION_SPAWN:
		return "SPAWN";
	case ACTION_CLOSE:
		return "CLOSE";
	case ACTION_FOCUS_NEXT:
		return "FOCUS_NEXT";
	case ACTION_FOCUS_PREV:
		return "FOCUS_PREV";
	case ACTION_FOCUS_MASTER:
		return "FOCUS_MASTER";
	case ACTION_SWAP_NEXT:
		return "SWAP_NEXT";
	case ACTION_SWAP_PREV:
		return "SWAP_PREV";
	case ACTION_SHRINK_MASTER:
		return "SHRINK_MASTER";
	case ACTION_EXPAND_MASTER:
		return "EXPAND_MASTER";
	case ACTION_INC_MASTER:
		return "INC_MASTER";
	case ACTION_DEC_MASTER:
		return "DEC_MASTER";
	case ACTION_TOGGLE_FLOAT:
		return "TOGGLE_FLOAT";
	case ACTION_TOGGLE_FULLSCREEN:
		return "TOGGLE_FULLSCREEN";
	case ACTION_MOVE:
		return "MOVE";
	case ACTION_RESIZE:
		return "RESIZE";
	case ACTION_RESIZE_DELTA:
		return "RESIZE_DELTA";
	case ACTION_EXIT:
		return "EXIT";
	case ACTION_WORKSPACE:
		return "WORKSPACE";
	case ACTION_MOVE_TO_WORKSPACE:
		return "MOVE_TO_WORKSPACE";
	case ACTION_MOVE_TO_WORKSPACE_FOLLOW:
		return "MOVE_TO_WORKSPACE_FOLLOW";
	case ACTION_FOCUS_LAST:
		return "FOCUS_LAST";
	case ACTION_RESTART:
		return "RESTART";
	case ACTION_TOGGLE_STICKY:
		return "TOGGLE_STICKY";
	case ACTION_FOCUS_MRU_NEXT:
		return "FOCUS_MRU_NEXT";
	case ACTION_FOCUS_MRU_PREV:
		return "FOCUS_MRU_PREV";
	case ACTION_NEXT_WORKSPACE:
		return "NEXT_WORKSPACE";
	case ACTION_PREV_WORKSPACE:
		return "PREV_WORKSPACE";
	case ACTION_FOCUS_OUTPUT:
		return "FOCUS_OUTPUT";
	case ACTION_MOVE_TO_OUTPUT:
		return "MOVE_TO_OUTPUT";
	case ACTION_LAYOUT_VERB:
		return "LAYOUT_VERB";
	case ACTION_NEXT_LAYOUT:
		return "NEXT_LAYOUT";
	case ACTION_PREV_LAYOUT:
		return "PREV_LAYOUT";
	case ACTION_RELOAD_CONFIG:
		return "RELOAD_CONFIG";
	case ACTION_TOGGLE_SCRATCHPAD:
		return "TOGGLE_SCRATCHPAD";
	case ACTION_NAMED_SCRATCHPAD:
		return "NAMED_SCRATCHPAD";
	case ACTION_SUBMAP:
		return "SUBMAP";
	}
	return "NONE";
}

bool action_kind_from_name(const char *name, enum ActionKind *out)
{
	static const struct {
		const char *name;
		enum ActionKind kind;
	} table[] = {
		{ "NONE", ACTION_NONE },
		{ "SPAWN", ACTION_SPAWN },
		{ "CLOSE", ACTION_CLOSE },
		{ "FOCUS_NEXT", ACTION_FOCUS_NEXT },
		{ "FOCUS_PREV", ACTION_FOCUS_PREV },
		{ "FOCUS_MASTER", ACTION_FOCUS_MASTER },
		{ "SWAP_NEXT", ACTION_SWAP_NEXT },
		{ "SWAP_PREV", ACTION_SWAP_PREV },
		{ "SHRINK_MASTER", ACTION_SHRINK_MASTER },
		{ "EXPAND_MASTER", ACTION_EXPAND_MASTER },
		{ "INC_MASTER", ACTION_INC_MASTER },
		{ "DEC_MASTER", ACTION_DEC_MASTER },
		{ "TOGGLE_FLOAT", ACTION_TOGGLE_FLOAT },
		{ "TOGGLE_FULLSCREEN", ACTION_TOGGLE_FULLSCREEN },
		{ "MOVE", ACTION_MOVE },
		{ "RESIZE", ACTION_RESIZE },
		{ "RESIZE_DELTA", ACTION_RESIZE_DELTA },
		{ "EXIT", ACTION_EXIT },
		{ "WORKSPACE", ACTION_WORKSPACE },
		{ "MOVE_TO_WORKSPACE", ACTION_MOVE_TO_WORKSPACE },
		{ "MOVE_TO_WORKSPACE_FOLLOW", ACTION_MOVE_TO_WORKSPACE_FOLLOW },
		{ "FOCUS_LAST", ACTION_FOCUS_LAST },
		{ "RESTART", ACTION_RESTART },
		{ "TOGGLE_STICKY", ACTION_TOGGLE_STICKY },
		{ "FOCUS_MRU_NEXT", ACTION_FOCUS_MRU_NEXT },
		{ "FOCUS_MRU_PREV", ACTION_FOCUS_MRU_PREV },
		{ "NEXT_WORKSPACE", ACTION_NEXT_WORKSPACE },
		{ "PREV_WORKSPACE", ACTION_PREV_WORKSPACE },
		{ "FOCUS_OUTPUT", ACTION_FOCUS_OUTPUT },
		{ "MOVE_TO_OUTPUT", ACTION_MOVE_TO_OUTPUT },
		{ "LAYOUT_VERB", ACTION_LAYOUT_VERB },
		{ "NEXT_LAYOUT", ACTION_NEXT_LAYOUT },
		{ "PREV_LAYOUT", ACTION_PREV_LAYOUT },
		{ "RELOAD_CONFIG", ACTION_RELOAD_CONFIG },
		{ "TOGGLE_SCRATCHPAD", ACTION_TOGGLE_SCRATCHPAD },
		{ "NAMED_SCRATCHPAD", ACTION_NAMED_SCRATCHPAD },
		{ "SUBMAP", ACTION_SUBMAP },
	};
	size_t i;

	if (name == NULL || out == NULL)
		return false;
	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (strcmp(name, table[i].name) == 0) {
			*out = table[i].kind;
			return true;
		}
	}
	return false;
}
