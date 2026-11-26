/* SPDX-License-Identifier: 0BSD */
/*
 * Layout name <-> kind mapping. Intentionally tiny and free of any
 * dependency beyond layout.h (forward decls of Output / Window only)
 * so the parser-only test binary can link this without pulling in
 * layout.c's apply chain (which transitively reaches window.c +
 * wayland-client). config.c uses this for parse_layout_name and the
 * (layout NAME) / (layouts ...) error messages; status.c reverse-maps
 * kind to name for the broadcast; wm.c walks the table to enumerate
 * compiled-in kinds and dispatches via a switch on kind for the
 * actual init.
 *
 * Adding a layout: drop a row here, add a case in
 * wm.c:layout_init_from_kind, ship the apply/init in layout.c. The
 * compile-time per-layout opt-out gates the row here AND the case
 * in wm.c with the same #ifndef NM_NO_<KIND>; the linker drops the
 * unreferenced apply/init automatically.
 */
#include "layout.h"

const struct LayoutDescriptor layout_descriptors[] = {
	{ "tall", LAYOUT_TALL },
#ifndef NM_NO_BSP
	{ "bsp", LAYOUT_BSP },
#endif
#ifndef NM_NO_DECK
	{ "deck", LAYOUT_DECK },
#endif
#ifndef NM_NO_MIRROR_TALL
	{ "mirror-tall", LAYOUT_MIRROR_TALL },
#endif
#ifndef NM_NO_THREE_COLUMNS
	{ "three-columns", LAYOUT_THREE_COLUMNS },
#endif
#ifndef NM_NO_GRID
	{ "grid", LAYOUT_GRID },
#endif
#ifndef NM_NO_TABBED
	{ "tabbed", LAYOUT_TABBED },
#endif
#ifndef NM_NO_SPIRAL
	{ "spiral", LAYOUT_SPIRAL },
#endif
#ifndef NM_NO_TWO_PANE
	{ "two-pane", LAYOUT_TWO_PANE },
#endif
#ifndef NM_NO_RESIZABLE_TALL
	{ "resizable-tall", LAYOUT_RESIZABLE_TALL },
#endif
};
const size_t layout_descriptors_n =
	sizeof(layout_descriptors) / sizeof(layout_descriptors[0]);
