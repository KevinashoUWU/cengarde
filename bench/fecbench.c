// fecbench: the cost of the upload's erasure codes alone (story 021), in µs per
// source packet, and what that is at 10 Mbit/s of 1200-byte packets (1042 a
// second): the XOR parity of redund_spare = parity|mix (fec.h), Reed-Solomon on
// Leopard-RS (rs.h) and the sliding-window RLC (rlc.h). No sockets, no engine:
// the sender's and the receiver's functions as client.c and server.c call them.
//
//   encode   per source packet: what the sender does for it and its share of the
//            repairs (XOR: the group's XOR; RS: the copy into the block and its
//            share of leo_encode at the close; RLC: the copy into the window and
//            its share of the repairs, each over the whole window)
//   decode   per source packet, the receiver: with nothing lost (each repair
//            checked against the ring and dropped), and with one or m lost in
//            every block (RS) or per repair interval (RLC; XOR: one per group)
//
// usage: fecbench [seconds-per-case (1)]
// Build (bench/lab.sh build does it, after make -C engine):
//   gcc -O2 -I engine/src -I engine/third_party/leopard -o bench/bin/fecbench
//       bench/fecbench.c engine/src/rs.c engine/obj/leopard-plain/*.o -lpthread
// Leopard's portable path (as on armhf or MIPS): make -C engine
// LEO_PORTABLE=-DCG_LEO_PORTABLE, and engine/obj/leopard-plain-portable/*.o.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fec.h"
#include "gf256.h"
#include "repair.h"
#include "rlc.h"
#include "rs.h"

#define PLEN 1200
#define PPS10 (10e6 / 8 / PLEN)

static double secs = 1;
static struct cg_gf G;
static struct cg_rs_tx RT;
static struct cg_rs_rx RR;
static struct cg_rs_scratch RX;
static struct cg_rlc_tx LT;
static struct cg_rlc_rx LR;
static struct cg_fec_tx FT;
static struct cg_fec_rx FR;
static uint32_t r_seq[CG_REP_RING];
static uint16_t r_len[CG_REP_RING];
static uint8_t r_have[CG_REP_RING], r_pidx[CG_REP_RING];
static uint8_t r_mem[CG_REP_RING * CG_SYM_PLEN];
static uint8_t pay[PLEN];
static volatile unsigned sink;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static struct cg_rep_ring ring(void)
{
	struct cg_rep_ring r = { r_seq, r_len, r_have, r_mem };

	return r;
}

static void ring_put(uint32_t seq)
{
	uint32_t i = seq & (CG_REP_RING - 1);

	r_seq[i] = seq;
	r_len[i] = PLEN;
	r_have[i] = 1;
	memcpy(r_mem + (size_t)i * CG_SYM_PLEN, pay, PLEN);
}

static void report(const char *what, double t, unsigned long n)
{
	double us = t / n * 1e6;

	printf("  %-46s %8.3f µs/pkt  %6.2f %% of a core at 10 Mbit/s\n", what, us, us * PPS10 / 1e4);
}

/* XOR: groups of k (redund_spare = parity's), the receiver rebuilding one per group. */
static void bench_xor(unsigned k, int lose)
{
	uint8_t par[CG_FEC_HDR + CG_FEC_PLEN_MAX], out[CG_FEC_PLEN_MAX];
	unsigned long n = 0;
	double t0 = now(), te = 0, td = 0;
	uint32_t seq = 0;
	char name[64];

	memset(&FT, 0, sizeof(FT));
	memset(&FR, 0, sizeof(FR));
	FR.seq = r_seq, FR.len = r_len, FR.have = r_have, FR.mem = r_mem, FR.pidx = r_pidx;
	memset(r_have, 0, sizeof(r_have));
	memset(r_pidx, 0, sizeof(r_pidx));
	while (now() - t0 < secs) {
		double a = now(), b;
		unsigned plen;
		uint32_t s;
		uint16_t l;
		int slot;

		for (unsigned i = 0; i < k; i++)
			cg_fec_grp_add(&FT.g[0], seq + i, 0, pay, PLEN, 0);
		plen = cg_fec_grp_close(&FT.g[0], par);
		b = now();
		te += b - a;
		for (unsigned i = 0; i < k; i++)
			if (!(lose && i == 1))
				cg_fec_rx_put(&FR, seq + i, pay, PLEN);
		slot = cg_fec_rx_parity(&FR, par, plen);
		sink += (unsigned)cg_fec_rx_try(&FR, slot, out, &s, &l);
		td += now() - b;
		seq += k;
		n += k;
	}
	snprintf(name, sizeof(name), "XOR k %u encode", k);
	report(name, te, n);
	snprintf(name, sizeof(name), "XOR k %u decode (keep, %s)", k, lose ? "1 lost a group" : "none lost");
	report(name, td, n);
}

/* RS: blocks of k with m repairs; the receiver loses `lose` members a block. */
static void bench_rs(unsigned k, unsigned m, unsigned lose)
{
	struct cg_rep_ring rg = ring();
	struct cg_rep_hdr h;
	uint8_t *sym[CG_RS_MMAX];
	uint32_t out[CG_RS_KMAX];
	unsigned long n = 0, rec = 0;
	double t0 = now(), te = 0, td = 0;
	uint32_t seq = 0;
	unsigned bytes;
	char name[80];

	memset(&RR, 0, sizeof(RR));
	memset(r_have, 0, sizeof(r_have));
	while (now() - t0 < secs) {
		double a = now(), b;
		unsigned got;

		for (unsigned i = 0; i < k; i++)
			cg_rs_tx_add(&RT, seq + i, pay, PLEN, m, 0);
		got = cg_rs_tx_close(&RT, &h, sym, &bytes);
		b = now();
		te += b - a;
		if (got != m) {
			printf("FAIL: rs encode\n");
			exit(1);
		}
		for (unsigned i = 0; i < k; i++)
			if (i >= lose)
				ring_put(seq + i);
		for (unsigned j = 0; j < m; j++) {
			h.idx = (uint16_t)j;
			rec += cg_rs_rx_repair(&RR, &RX, &rg, &h, sym[j], bytes, out, CG_RS_KMAX);
		}
		td += now() - b;
		seq += 64;
		n += k;
	}
	if (rec != n / k * lose) {
		printf("FAIL: rs decode %lu of %lu\n", rec, n / k * lose);
		exit(1);
	}
	snprintf(name, sizeof(name), "RS k %u m %u encode", k, m);
	report(name, te, n);
	snprintf(name, sizeof(name), "RS k %u m %u decode (%u lost a block)", k, m, lose);
	report(name, td, n);
}

/* RLC: a window of w, a repair every w/m packets; the receiver loses one
 * packet in every `lose` intervals between repairs (0: none). */
static void bench_rlc(unsigned w, unsigned m, unsigned lose)
{
	struct cg_rep_ring rg = ring();
	struct cg_rep_hdr h;
	uint8_t sym[CG_SYM_MAX];
	uint32_t out[CG_RLC_EQ];
	unsigned long n = 0, rec = 0, lost = 0;
	double t0 = now(), te = 0, td = 0;
	uint32_t seq = 0;
	unsigned every = w / m, rate = 256 * m / w, iv = 0;
	char name[80];

	memset(&LT, 0, sizeof(LT));
	memset(&LR, 0, sizeof(LR));
	memset(r_have, 0, sizeof(r_have));
	while (now() - t0 < secs) {
		iv++;
		for (unsigned i = 0; i < every; i++, seq++) {
			double a = now(), b;
			unsigned owed, sl = 0;

			cg_rlc_tx_add(&LT, seq, pay, PLEN, 0);
			owed = cg_rlc_tx_owed(&LT, rate);
			if (owed)
				sl = cg_rlc_tx_repair(&G, &LT, w, &h, sym);
			b = now();
			te += b - a;
			if (lose && i == 0 && iv % lose == 0) {
				lost++;
			} else {
				ring_put(seq);
				rec += cg_rlc_rx_data(&G, &LR, &rg, seq, out, CG_RLC_EQ);
			}
			if (sl)
				rec += cg_rlc_rx_repair(&G, &LR, &rg, &h, sym, sl, out, CG_RLC_EQ);
			td += now() - b;
			n++;
		}
	}
	snprintf(name, sizeof(name), "RLC w %u, %u per %u, encode", w, m, w);
	report(name, te, n);
	snprintf(name, sizeof(name), "RLC w %u decode (1 lost per %u; %.0f %% back)", w, lose * every,
		 lost ? 100.0 * rec / lost : 100.0);
	report(name, td, n);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		secs = atof(argv[1]);
	for (unsigned i = 0; i < PLEN; i++)
		pay[i] = (uint8_t)(i * 7 + 3);
	cg_gf_init(&G);
	if (cg_rs_init() < 0) {
		printf("no Leopard\n");
		return 1;
	}
	printf("1200-byte packets; %% of a core at 10 Mbit/s = %.0f packets a second\n", PPS10);
	bench_xor(7, 0);
	bench_xor(7, 1);
	bench_rs(21, 3, 0);
	bench_rs(21, 3, 1);
	bench_rs(21, 3, 3);
	bench_rs(32, 5, 5);
	bench_rs(64, 8, 8);
	bench_rlc(21, 3, 0);
	bench_rlc(21, 3, 2);
	bench_rlc(32, 5, 0);
	bench_rlc(32, 5, 2);
	bench_rlc(64, 8, 2);
	return 0;
}
