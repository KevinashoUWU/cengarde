/* zstd on the reliable lane (zlane.h, story 031): datagrams through a
 * compressor and a decompressor that see them in order.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "zlane.h"

void test_zlane(void)
{
#ifdef CG_ZSTD
	ZSTD_CCtx *c = cg_zl_cnew();
	ZSTD_DCtx *d = cg_zl_dnew();
	char msg[512];
	uint8_t z[1024], back[1024];
	size_t first = 0, last = 0, in = 0, out = 0;

	CHECK(c != NULL && d != NULL);
	/* a sensor's JSON, one reading a datagram: the same keys every time */
	for (int i = 0; i < 200; i++) {
		size_t n = (size_t)snprintf(msg, sizeof(msg),
					    "{\"site\":\"manto-verde\",\"sensor\":\"pump-%02d\",\"ts\":%d,"
					    "\"pressure_kpa\":%d.%d,\"flow_lps\":%d.%d,\"temp_c\":%d,\"ok\":true}",
					    i % 8, 1730000000 + i, 300 + i % 17, i % 10, 40 + i % 5, (i * 7) % 10, 20 + i % 9);
		size_t k = cg_zl_comp(c, (const uint8_t *)msg, n, z, sizeof(z)), b;

		CHECK(k > 0 && k != (size_t)-1);
		b = cg_zl_decomp(d, z, k, back, sizeof(back));
		CHECK_EQ(b, n);
		CHECK(b == n && !memcmp(back, msg, n));
		if (!i)
			first = k;
		last = k;
		in += n;
		out += k;
	}
	/* the history learns the shape: later readings far smaller than the
	 * first, and the whole a fraction of the raw */
	CHECK(last * 3 < first);
	CHECK(out * 3 < in);
	/* an empty datagram is one too */
	{
		size_t k = cg_zl_comp(c, (const uint8_t *)"", 0, z, sizeof(z));

		CHECK(k != (size_t)-1);
		CHECK_EQ(cg_zl_decomp(d, z, k, back, sizeof(back)), 0);
		/* and the history goes on after it */
		k = cg_zl_comp(c, (const uint8_t *)msg, strlen(msg), z, sizeof(z));
		CHECK(k > 0 && k != (size_t)-1);
		CHECK_EQ(cg_zl_decomp(d, z, k, back, sizeof(back)), strlen(msg));
	}
	/* too small a room: refused, not cut */
	{
		ZSTD_CCtx *c2 = cg_zl_cnew();

		CHECK_EQ(cg_zl_comp(c2, (const uint8_t *)msg, strlen(msg), z, 4), (size_t)-1);
		ZSTD_freeCCtx(c2);
	}
	/* bytes that are not a datagram of this stream are refused */
	memset(z, 0xa5, 64);
	CHECK_EQ(cg_zl_decomp(d, z, 64, back, sizeof(back)), (size_t)-1);
	ZSTD_freeCCtx(c);
	ZSTD_freeDCtx(d);
#endif
}
