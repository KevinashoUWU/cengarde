/* linkowd.h: what each link adds to a GRIT message's delay (task #77).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "linkowd.h"
#include "test.h"

#define S 1000000ull

/* Two links 15 ms apart: a message by the longer one reads as the shorter
 * one's, once both have a least; a queue on both still shows; no link, no
 * correction. */
static void test_lowd_paths(void)
{
	struct cg_lowd o;
	const int32_t off = 2000000000; /* the two clocks' offset: any */
	int32_t d;
	int ok;

	cg_lowd_init(&o);
	/* the first message by each link: its own least */
	CHECK_EQ(cg_lowd_take(&o, 0, off + 10000, 1 * S), off + 10000);
	d = cg_lowd_take(&o, 1, off + 25000, 1 * S + 1000);
	CHECK_EQ(d, off + 10000); /* the 15 ms of the longer path taken off */
	CHECK_EQ(cg_lowd_extra(&o, 1), 15000);
	CHECK_EQ(cg_lowd_extra(&o, 0), 0);
	/* a queue of 8 ms on the longer link: it shows over its own least */
	CHECK_EQ(cg_lowd_take(&o, 1, off + 33000, 1 * S + 2000), off + 18000);
	/* and on the shorter one */
	CHECK_EQ(cg_lowd_take(&o, 0, off + 18000, 1 * S + 3000), off + 18000);
	/* no link: as it came */
	CHECK_EQ(cg_lowd_take(&o, CG_LOWD_NOLINK, off + 99000, 1 * S + 4000), off + 99000);
	CHECK_EQ(o.samples, 4);
	CHECK_EQ(o.corrected, 2);
	(void)cg_lowd_base(&o, 7, &ok);
	CHECK(!ok);
	CHECK_EQ(cg_lowd_extra(&o, 7), 0);
}

/* The least is over the last 5 to 10 s: an old least goes, the clock's
 * wrap of 32 bits is a difference like any other. */
static void test_lowd_window(void)
{
	struct cg_lowd o;
	int ok;

	cg_lowd_init(&o);
	cg_lowd_take(&o, 0, 10000, 1 * S);
	cg_lowd_take(&o, 1, 25000, 1 * S);
	/* the path of link 1 grows longer (a handover): within a bucket its
	 * least stays, then it follows */
	cg_lowd_take(&o, 1, 40000, 2 * S);
	CHECK_EQ(cg_lowd_extra(&o, 1), 15000);
	cg_lowd_take(&o, 0, 10000, 1 * S + CG_LOWD_WIN_US); /* a new bucket */
	cg_lowd_take(&o, 1, 40000, 1 * S + CG_LOWD_WIN_US + 1);
	CHECK_EQ(cg_lowd_extra(&o, 1), 15000); /* the last bucket still has 25 */
	cg_lowd_take(&o, 0, 10000, 1 * S + 2 * CG_LOWD_WIN_US);
	cg_lowd_take(&o, 1, 40000, 1 * S + 2 * CG_LOWD_WIN_US + 1);
	CHECK_EQ(cg_lowd_extra(&o, 1), 30000);
	/* a link idle for two buckets is forgotten */
	cg_lowd_take(&o, 0, 10000, 1 * S + 5 * CG_LOWD_WIN_US);
	cg_lowd_base(&o, 1, &ok);
	CHECK(!ok);
	/* the 32-bit clock wraps between two samples */
	cg_lowd_init(&o);
	cg_lowd_take(&o, 0, INT32_MAX - 5000, 1 * S);
	CHECK_EQ(cg_lowd_take(&o, 1, (int32_t)((uint32_t)INT32_MAX + 10000), 1 * S), INT32_MAX - 5000);
	CHECK_EQ(cg_lowd_extra(&o, 1), 15000);
}

/* An ACK's delay: the worst of the links that brought a quarter or more of
 * its messages, so a link with a few messages and no queue does not hide
 * the main link's queue. */
static void test_lowd_ack(void)
{
	struct cg_lowd_ack a;
	int32_t d = 0;

	memset(&a, 0, sizeof(a));
	CHECK(!cg_lowd_ack_get(&a, &d)); /* nothing */
	for (int i = 0; i < 9; i++)
		cg_lowd_ack_add(&a, 0, 30000 + i); /* the main link, 20 ms of queue */
	cg_lowd_ack_add(&a, 1, 10000);          /* one message by a clear link */
	CHECK(cg_lowd_ack_get(&a, &d));
	CHECK_EQ(d, 30000); /* the main link's least, not 10 */
	CHECK(!cg_lowd_ack_get(&a, &d)); /* it started again */
	cg_lowd_ack_add(&a, 0, 30000);
	cg_lowd_ack_add(&a, 1, 12000);
	cg_lowd_ack_add(&a, CG_LOWD_NOLINK, 1000); /* no link: not counted */
	CHECK(cg_lowd_ack_get(&a, &d));
	CHECK_EQ(d, 30000);
	cg_lowd_ack_add(&a, 2, 15000);
	CHECK(cg_lowd_ack_get(&a, &d));
	CHECK_EQ(d, 15000);
}

void test_linkowd(void)
{
	test_lowd_paths();
	test_lowd_window();
	test_lowd_ack();
}
