/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>

#include "cbpf.h"
#include "steer.h"
#include "test.h"

/* The interpreter itself refuses what the kernel would, and runs what it
 * accepts: a sanity check of the checker the steering tests rely on. */
static void test_cbpf(void)
{
	static const uint8_t pkt[5] = { 7, 8, 9, 10, 11 };
	struct sock_filter back[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 4, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, 1),
		BPF_STMT(BPF_RET | BPF_A, 0),
	};
	struct sock_filter add[] = {
		BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 1),
		BPF_STMT(BPF_MISC | BPF_TAX, 0),
		BPF_STMT(BPF_LD | BPF_IMM, 100),
		BPF_STMT(BPF_ALU | BPF_ADD | BPF_X, 0),
		BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 1),
		BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 109, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, 0),
		BPF_JUMP(BPF_JMP | BPF_JA, 1, 0, 0),
		BPF_STMT(BPF_RET | BPF_K, 1),
		BPF_STMT(BPF_RET | BPF_A, 0),
	};
	struct sock_filter past[] = {
		BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 4),
		BPF_STMT(BPF_RET | BPF_K, 9),
	};

	CHECK(cbpf_check(back, 4) == NULL);
	CHECK_EQ(cbpf_run(back, 4, pkt, 4), 4); /* len 4: jumps over RET #1 */
	CHECK_EQ(cbpf_run(back, 4, pkt, 3), 1);
	CHECK(cbpf_check(add, 10) == NULL);
	CHECK_EQ(cbpf_run(add, 10, pkt, 4), 109); /* 100 + 8 + 1 */
	CHECK(cbpf_check(past, 2) == NULL);
	CHECK_EQ(cbpf_run(past, 2, pkt, 4), 0); /* past the end: 0, as the kernel */
	CHECK_EQ(cbpf_run(past, 2, pkt, 5), 9);

	back[1].jt = 2; /* to index 4, past the end */
	CHECK(cbpf_check(back, 4) != NULL);
	back[1].jt = 1;
	CHECK(cbpf_check(back, 3) != NULL); /* jumps out, and no RET last */
	add[7].k = 2;
	CHECK(cbpf_check(add, 10) != NULL); /* JA past the end */
	add[7].k = 1;
	add[4].code = BPF_ALU | BPF_MUL | BPF_K;
	CHECK(cbpf_check(add, 10) != NULL); /* not an opcode it knows */
	CHECK(cbpf_check(add, 0) != NULL);
	CHECK(cbpf_check(add, BPF_MAXINSNS + 1) != NULL);
}

/* Every lane count, against cg_steer_ref: each length class around the
 * header (and a full datagram), every version byte, every link byte. */
static void test_steer_v3(void)
{
	static const size_t lens[] = { 0, 1, 3, 4, 5, 16, 23, 24, 25, 48, 1400 };
	static const unsigned lanes_ok[] = { 1, 2, 4, 8, 16 };
	static const unsigned lanes_bad[] = { 0, 3, 5, 6, 12, 32, 64 };
	struct sock_filter prog[CG_STEER_MAX];
	uint8_t pkt[1400];

	for (size_t i = 0; i < sizeof(pkt); i++)
		pkt[i] = (uint8_t)(i * 37 + 11);
	for (size_t li = 0; li < sizeof(lanes_ok) / sizeof(lanes_ok[0]); li++) {
		unsigned L = lanes_ok[li], seen[CG_MAX_LANES + 1];
		int n = cg_steer_prog(prog, CG_STEER_MAX, L);

		CHECK(n > 0 && n <= CG_STEER_MAX && n <= BPF_MAXINSNS);
		if (n <= 0)
			continue;
		CHECK(cbpf_check(prog, n) == NULL);
		memset(seen, 0, sizeof(seen));
		for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
			uint32_t wrong = 0, past = 0;

			for (unsigned v = 0; v < 256; v++) {
				for (unsigned link = 0; link < 256; link++) {
					uint32_t got, want;

					pkt[0] = (uint8_t)v;
					pkt[CG_LINK_OFF] = (uint8_t)link;
					got = cbpf_run(prog, n, pkt, (uint32_t)lens[k]);
					want = cg_steer_ref(pkt, lens[k], L);
					wrong += got != want;
					past += got > L;
					if (got <= L)
						seen[got]++;
				}
			}
			CHECK_EQ(wrong, 0);
			CHECK_EQ(past, 0); /* never outside the group */
		}
		/* Every lane and the junk socket get something. */
		for (unsigned l = 0; l <= L; l++)
			CHECK(seen[l] > 0);
	}
	/* Concretely, with 8 lanes. */
	CHECK_EQ(cg_steer_prog(prog, CG_STEER_MAX, 8), 9);
	pkt[0] = (uint8_t)(CG_PROTO_VERSION << 4 | CG_T_DATA);
	pkt[CG_LINK_OFF] = 2;
	CHECK_EQ(cbpf_run(prog, 9, pkt, 1400), 2);
	CHECK_EQ(cbpf_run(prog, 9, pkt, CG_HDR_LEN), 2);
	CHECK_EQ(cbpf_run(prog, 9, pkt, CG_HDR_LEN - 1), 8); /* short: junk */
	pkt[CG_LINK_OFF] = 13;
	CHECK_EQ(cbpf_run(prog, 9, pkt, 1400), 5);
	pkt[0] = (uint8_t)((CG_PROTO_VERSION + 1) << 4 | CG_T_DATA);
	CHECK_EQ(cbpf_run(prog, 9, pkt, 1400), 8); /* the next protocol version: junk */
	pkt[0] = 4; /* a WireGuard data message sent to the wrong port */
	CHECK_EQ(cbpf_run(prog, 9, pkt, 1400), 8);

	for (size_t li = 0; li < sizeof(lanes_bad) / sizeof(lanes_bad[0]); li++)
		CHECK_EQ(cg_steer_prog(prog, CG_STEER_MAX, lanes_bad[li]), -1);
	CHECK_EQ(cg_steer_prog(prog, 8, 8), -1); /* no room */
	CHECK(cg_lanes_valid(1) && cg_lanes_valid(16) && !cg_lanes_valid(0) && !cg_lanes_valid(24));
}

void test_steer(void)
{
	test_cbpf();
	test_steer_v3();
}
