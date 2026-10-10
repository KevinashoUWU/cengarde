/* Cookies and the HELLO budget of protocol 4 (proto.h).
 *
 * A server answers a probe it cannot take yet (no session for it, a link it
 * has no path for, or an address the path never used) with a HELLO to the
 * probe's source address carrying a cookie, and takes the next probe of that
 * link only if it echoes the cookie. The cookie is SipHash-2-4, under a
 * random key of the current 30-second epoch, over the client, the session,
 * the link and that address; the keys of the current and the previous epoch
 * verify, so a cookie lives 30 to 60 seconds. Nothing is kept per client:
 * a probe captured earlier, or sent again from another address, cannot echo
 * a valid cookie, since the probe's MAC covers it and only the client holds
 * the key.
 *
 * A session that timed out can be taken up again by a probe captured less
 * than a cookie's life before and sent again from the client's own address
 * (the cookie is bound to it): the same session, towards the same client,
 * with the wishes it had then until its next probe; what it sent meanwhile
 * goes to WireGuard again, which drops it by its own counters.
 *
 * HELLOs go only to probes whose MAC verified, are never larger than the
 * probe, and come out of a token bucket (cg_bucket), so the server cannot be
 * used to send much anywhere.
 *
 * Pure: the caller brings the clock and the random key bytes.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_COOKIE_H
#define CG_COOKIE_H

#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "siphash.h"

#define CG_COOKIE_EPOCH_MS 30000

struct cg_cookie_keys {
	uint8_t key[2][CG_SIPHASH_KEY_LEN]; /* [0] the current epoch, [1] the previous one */
	uint64_t epoch;                     /* of key[0] */
	uint8_t have;                       /* keys set: 0, 1 or 2 */
};

/* Whether the keys have to move to the epoch of now_ms (then call
 * cg_cookie_rotate with fresh random bytes). */
static inline int cg_cookie_due(const struct cg_cookie_keys *k, uint64_t now_ms)
{
	return !k->have || now_ms / CG_COOKIE_EPOCH_MS != k->epoch;
}

static inline void cg_cookie_rotate(struct cg_cookie_keys *k, uint64_t now_ms, const uint8_t fresh[CG_SIPHASH_KEY_LEN])
{
	uint64_t e = now_ms / CG_COOKIE_EPOCH_MS;

	/* The current key stays valid for one more epoch, never for two. */
	if (k->have && e == k->epoch + 1) {
		memcpy(k->key[1], k->key[0], CG_SIPHASH_KEY_LEN);
		k->have = 2;
	} else {
		k->have = 1;
	}
	memcpy(k->key[0], fresh, CG_SIPHASH_KEY_LEN);
	k->epoch = e;
}

/* The cookie of client, session and link for a probe from `from` under key;
 * never 0, which says "no cookie". */
static inline uint32_t cg_cookie(const uint8_t key[CG_SIPHASH_KEY_LEN], uint32_t client, uint32_t session,
				 uint8_t link, const struct sockaddr_storage *from)
{
	struct cg_siphash s;
	uint8_t b[28];
	size_t n = 0;
	uint32_t c;

	memcpy(b, &client, 4);
	memcpy(b + 4, &session, 4);
	b[8] = link;
	b[9] = (uint8_t)from->ss_family;
	n = 10;
	if (from->ss_family == AF_INET) {
		const struct sockaddr_in *a = (const struct sockaddr_in *)from;

		memcpy(b + n, &a->sin_port, 2);
		memcpy(b + n + 2, &a->sin_addr, 4);
		n += 6;
	} else if (from->ss_family == AF_INET6) {
		const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)from;

		memcpy(b + n, &a->sin6_port, 2);
		memcpy(b + n + 2, &a->sin6_addr, 16);
		n += 18;
	}
	cg_siphash_init(&s, key);
	cg_siphash_update(&s, b, n);
	c = (uint32_t)cg_siphash_final(&s);
	return c ? c : 1;
}

/* The cookie a HELLO hands out now (keys current: cg_cookie_due false). */
static inline uint32_t cg_cookie_make(const struct cg_cookie_keys *k, uint32_t client, uint32_t session, uint8_t link,
				      const struct sockaddr_storage *from)
{
	return cg_cookie(k->key[0], client, session, link, from);
}

/* Whether a probe's cookie was handed out for it, in this epoch or the one
 * before (keys current). */
static inline int cg_cookie_ok(const struct cg_cookie_keys *k, uint32_t cookie, uint32_t client, uint32_t session,
			       uint8_t link, const struct sockaddr_storage *from)
{
	if (!cookie || !k->have)
		return 0;
	if (cookie == cg_cookie(k->key[0], client, session, link, from))
		return 1;
	return k->have == 2 && cookie == cg_cookie(k->key[1], client, session, link, from);
}

/* A token bucket: up to burst at once, rate per second on average. */
struct cg_bucket {
	uint32_t milli;   /* tokens x 1000 */
	uint64_t last_ms; /* 0: full */
};

static inline int cg_bucket_take(struct cg_bucket *b, uint64_t now_ms, uint32_t rate, uint32_t burst)
{
	uint64_t t;

	if (!b->last_ms) {
		b->milli = burst * 1000;
		b->last_ms = now_ms ? now_ms : 1;
	} else if (now_ms > b->last_ms) {
		t = b->milli + (now_ms - b->last_ms) * rate;
		b->milli = t > (uint64_t)burst * 1000 ? burst * 1000 : (uint32_t)t;
		b->last_ms = now_ms;
	}
	if (b->milli < 1000)
		return 0;
	b->milli -= 1000;
	return 1;
}

#endif
