/* DiffServ in the protocontroller's sends (dscp.h, story 031).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "dscp.h"
#include "test.h"

void test_dscp(void)
{
	/* RFC 4594's real-time marks first, LE and CS1 last, the rest as is */
	CHECK_EQ(cg_ds_tier(46), CG_DS_PRIO); /* EF */
	CHECK_EQ(cg_ds_tier(34), CG_DS_PRIO); /* AF41: video calls */
	CHECK_EQ(cg_ds_tier(40), CG_DS_PRIO); /* CS5 */
	CHECK_EQ(cg_ds_tier(1), CG_DS_LOW);   /* LE */
	CHECK_EQ(cg_ds_tier(8), CG_DS_LOW);   /* CS1 */
	CHECK_EQ(cg_ds_tier(0), CG_DS_NORMAL);
	CHECK_EQ(cg_ds_tier(10), CG_DS_NORMAL); /* AF11 */
	CHECK_EQ(cg_ds_tier(26), CG_DS_NORMAL); /* AF31: streaming, not first */
	CHECK_EQ(cg_ds_tier(46 | 0x40), CG_DS_PRIO); /* only the 6 bits count */

	/* an EF flow under a quarter of the pacer's rate goes first; at it or
	 * over, as the rest (an app marking all it sends EF) */
	CHECK_EQ(cg_ds_flow_tier(46, 100000, 10000000), CG_DS_PRIO);
	CHECK_EQ(cg_ds_flow_tier(46, 2500000, 10000000), CG_DS_NORMAL);
	CHECK_EQ(cg_ds_flow_tier(46, 0, 0), CG_DS_NORMAL); /* no rate yet: never first over nothing */
	CHECK_EQ(cg_ds_flow_tier(1, 0, 10000000), CG_DS_LOW);
	CHECK_EQ(cg_ds_flow_tier(0, 9000000, 10000000), CG_DS_NORMAL);

	/* a lower-effort flow is owed a segment 250 ms after its last one */
	CHECK(!cg_ds_low_owed(1000000, 0, 1100000));
	CHECK(cg_ds_low_owed(1000000, 0, 1250000));
	CHECK(!cg_ds_low_owed(0, 2000000, 2100000)); /* never sent: from its start */
	CHECK(cg_ds_low_owed(0, 2000000, 2300000));
}
