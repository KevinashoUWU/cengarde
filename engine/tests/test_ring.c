/* The ring's pure parts and its single-thread behaviour; the atomics under
 * concurrency are in test_threads.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>
#include <string.h>

#include "ring.h"
#include "test.h"

static void put(struct cg_ring *r, uint32_t n, uint32_t first)
{
	uint32_t p = cg_ring_prod(r);

	for (uint32_t i = 0; i < n; i++)
		*(uint32_t *)cg_ring_at(r, p + i) = first + i;
	cg_ring_publish(r, n);
}

void test_ring(void)
{
	struct cg_ring r;
	_Atomic uint32_t blocked;
	struct cg_bell b;
	uint32_t got;

	/* Pure parts, across the wrap of the indices. */
	CHECK_EQ(cg_ring_count(5, 5), 0);
	CHECK_EQ(cg_ring_count(3, 0xfffffffdu), 6);
	CHECK_EQ(cg_ring_room_of(8, 3, 0xfffffffdu), 2);
	CHECK_EQ(cg_ring_room_of(8, 0xfffffffeu, 0xfffffff6u), 0);
	CHECK(cg_ring_size_ok(2) && cg_ring_size_ok(64) && cg_ring_size_ok(1u << 15));
	CHECK(!cg_ring_size_ok(0) && !cg_ring_size_ok(1) && !cg_ring_size_ok(48) && !cg_ring_size_ok(1u << 16));
	CHECK_EQ(cg_ring_init(&r, 48, 4), -1);

	/* Empty, full, batch publish, entry size rounded up. */
	CHECK_EQ(cg_ring_init(&r, 8, 3), 0);
	CHECK_EQ(r.esize, 8);
	CHECK_EQ(cg_ring_size(&r), 8);
	CHECK_EQ(cg_ring_avail(&r), 0);
	CHECK_EQ(cg_ring_room(&r, 1), 8);
	put(&r, 5, 100);
	CHECK_EQ(cg_ring_avail(&r), 5);
	CHECK_EQ(cg_ring_room(&r, 1), 3);
	put(&r, 3, 105);
	CHECK_EQ(cg_ring_room(&r, 1), 0);
	CHECK_EQ(cg_ring_avail(&r), 5); /* the consumer's cached prod: no reload while it has work */
	got = *(uint32_t *)cg_ring_at(&r, cg_ring_cons(&r));
	CHECK_EQ(got, 100);
	cg_ring_release(&r, 5);
	CHECK_EQ(cg_ring_avail(&r), 3); /* reloads prod once its cached copy shows nothing */
	/* The producer's cached cons is stale until it needs more room. */
	CHECK_EQ(r.cons_seen, 0);
	CHECK_EQ(cg_ring_room(&r, 1), 5);
	CHECK_EQ(r.cons_seen, 5);
	CHECK_EQ(*(uint32_t *)cg_ring_at(&r, cg_ring_cons(&r) + 2), 107);
	cg_ring_release(&r, 3);
	CHECK_EQ(cg_ring_avail(&r), 0);
	CHECK(!cg_ring_has_work_sc(&r));
	cg_ring_free(&r);

	/* Indices wrap at 2^32 with the entries in order. */
	CHECK_EQ(cg_ring_init(&r, 4, 4), 0);
	atomic_store(&r.prod, 0xfffffffeu);
	atomic_store(&r.cons, 0xfffffffeu);
	r.cons_seen = r.prod_seen = 0xfffffffeu;
	put(&r, 4, 7);
	CHECK_EQ(cg_ring_prod(&r), 2);
	CHECK_EQ(cg_ring_avail(&r), 4);
	CHECK_EQ(cg_ring_room(&r, 1), 0);
	for (uint32_t i = 0; i < 4; i++)
		CHECK_EQ(*(uint32_t *)cg_ring_at(&r, 0xfffffffeu + i), 7 + i);
	cg_ring_release(&r, 4);
	CHECK_EQ(cg_ring_cons(&r), 2);
	CHECK_EQ(cg_ring_room(&r, 4), 4);

	/* Space handshake, one thread: full stays blocked until the consumer
	 * releases, and the release clears it and writes the eventfd once. */
	CHECK_EQ(cg_bell_init(&b), 0);
	atomic_init(&blocked, 0);
	put(&r, 4, 0);
	CHECK_EQ(cg_ring_block(&r, &blocked), 1);
	CHECK(!cg_ring_unblocked(&blocked));
	CHECK_EQ(cg_ring_block(&r, &blocked), 1); /* still full */
	cg_ring_release(&r, 1);
	CHECK_EQ(cg_ring_unblock(&blocked, b.efd), 1);
	CHECK(cg_ring_unblocked(&blocked));
	CHECK_EQ(cg_ring_unblock(&blocked, b.efd), 0); /* once per episode */
	{
		uint64_t v = 0;

		CHECK_EQ(read(b.efd, &v, sizeof(v)), (long long)sizeof(v));
		CHECK_EQ(v, 1);
	}
	/* Room after all: not blocked. */
	CHECK_EQ(cg_ring_block(&r, &blocked), 0);
	CHECK(cg_ring_unblocked(&blocked));

	/* Bell: rings only a consumer that armed it, once. */
	CHECK_EQ(cg_bell_ring(&b), 0);
	cg_bell_arm(&b);
	CHECK_EQ(cg_bell_ring(&b), 1);
	CHECK_EQ(cg_bell_ring(&b), 0);
	cg_bell_drain(&b);
	CHECK_EQ(atomic_load(&b.sleeping), 0);
	cg_bell_arm(&b);
	CHECK(cg_ring_has_work_sc(&r)); /* the pre-sleep check sees the 3 entries left */
	cg_bell_disarm(&b);
	cg_bell_free(&b);
	cg_ring_free(&r);
}
