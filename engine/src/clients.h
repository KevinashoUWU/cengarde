/* Several clients (routers) on one server: which keys a packet may be under,
 * found from its client hint (proto.h, byte 2) before any MAC is computed.
 *
 * The hint is the first byte of a hash of the client's key (pair.h), so two
 * clients share one by chance (with 32 clients, about 2 pairs in all). The
 * index chains the clients of each hint in their order: a packet tries the
 * keys of its hint's chain only, one MAC in the common case and two or three
 * at worst, never the key of every client. A hint no client has costs
 * nothing but the lookup.
 *
 * Sessions are found by session id, which each client picks at random. Two
 * clients may pick the same one (by chance, or on purpose, since ids travel
 * in the clear): the session map is keyed by the id XORed with a random
 * value of the client's slot (cg_sesskey), drawn when the server starts, so
 * a client cannot aim at another's key, and a lookup also checks that the
 * session found belongs to that client.
 *
 * Pure, and with no allocation: the server rebuilds the index on every
 * reload.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_CLIENTS_H
#define CG_CLIENTS_H

#include <stdint.h>
#include <string.h>

/* [client] sections of a server; a cengarde-vps-setup server has at most 32
 * (one WireGuard port each). */
#define CG_MAX_CLIENTS 64
/* Sessions one client holds at once: a router that restarted keeps its old
 * session until it times out; a fifth replaces the one heard from least
 * recently, so a client restarting in a loop never fills the table. */
#define CG_CLIENT_SESSIONS 4

struct cg_hintidx {
	int8_t first[256];           /* hint -> its first client, -1: none */
	int8_t next[CG_MAX_CLIENTS]; /* the next client with the same hint, -1: none */
};

/* The index of clients 0..n-1 (n <= CG_MAX_CLIENTS) with hints hint[], for
 * those whose used[] is set; each chain in index order. */
static inline void cg_hintidx_build(struct cg_hintidx *x, const uint8_t *hint, const uint8_t *used, int n)
{
	memset(x->first, -1, sizeof(x->first));
	memset(x->next, -1, sizeof(x->next));
	for (int i = n - 1; i >= 0; i--)
		if (used[i]) {
			x->next[i] = x->first[hint[i]];
			x->first[hint[i]] = (int8_t)i;
		}
}

/* The session map's key for session id of the client whose slot drew mix. */
static inline uint32_t cg_sesskey(uint32_t mix, uint32_t id)
{
	return id ^ mix;
}

#endif
