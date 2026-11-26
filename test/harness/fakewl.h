/* SPDX-License-Identifier: 0BSD */
#ifndef NOMONAD_FAKEWL_H
#define NOMONAD_FAKEWL_H

#include <stdint.h>

enum FakewlPhase {
	FAKEWL_IDLE = 0,
	FAKEWL_MANAGE = 1,
	FAKEWL_RENDER = 2,
	FAKEWL_TEARDOWN = 3,
};

struct fake_proxy {
	void *user_data;
	uint32_t version;
	void (**implementation)(void);
	const char *iface;
	struct fake_proxy *next_dead;
	int dead;
};

extern enum FakewlPhase fakewl_phase;

void fakewl_reap(void);

#endif /* NOMONAD_FAKEWL_H */
