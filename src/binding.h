/* SPDX-License-Identifier: 0BSD */
/*
 * xkb and pointer binding registration.
 *
 * binding_register_from_config installs every defkeys/defmouse entry
 * from the supplied Config onto the seat. The Config is read but not
 * retained; each binding deep-copies its action.arg.
 *
 * binding_seat_cleanup destroys all bindings on the seat. Called from
 * seat_maybe_destroy and (later) hot reload.
 */
#ifndef NOMONAD_BINDING_H
#define NOMONAD_BINDING_H

struct Seat;
struct Config;
struct SubmapDef;

void binding_register_from_config(struct Seat *seat,
				  const struct Config *cfg);
void binding_seat_cleanup(struct Seat *seat);

/*
 * Install a submap's entries as the seat's only xkb bindings (mods=0).
 * Caller is expected to binding_seat_cleanup first; this only adds.
 * Pointer bindings are left untouched so a submap can still be
 * cancelled by clicking. No-op on NULL def or zero entries.
 */
void binding_register_submap(struct Seat *seat, const struct SubmapDef *def);

#endif /* NOMONAD_BINDING_H */
