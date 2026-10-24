/* Reed-Solomon block erasure code for redund_spare = rs (story 021), on
 * Leopard-RS (third_party/leopard: Christopher Taylor's FFT-based MDS
 * code over GF(2^8), BSD-3, built without libstdc++).
 *
 * A block is k source packets (DATA that went on the first link alone,
 * within CG_REP_SPAN sequences) and m repairs: any k of the k + m give
 * the k back. Leopard's terms: each symbol (a member's length and payload,
 * gf256.h) is padded to the block's longest rounded up to 64 bytes; k + m
 * at most 256 and m at most k (here k at most CG_REP_SPAN, m at most
 * CG_RS_MMAX); its work buffers come from the caller, sized once for the
 * largest block (CG_RS_WORK_ENC, CG_RS_WORK_DEC), never per packet.
 *
 * Sender (struct cg_rs_tx): the block's members copied as they go, the m
 * repairs computed at its close (k members, or its deadline). Receiver
 * (struct cg_rs_rx): a block with members missing keeps its repairs (a
 * pool of CG_RS_RXREP) until as many are there as missing, then decodes
 * (cg_rs_decode, with the scratch of struct cg_rs_scratch) and writes what
 * it rebuilds into the ring (repair.h). Without Leopard (make LEOPARD=0)
 * cg_rs_init fails and the rest does nothing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_RS_H
#define CG_RS_H

#include <stdint.h>

#include "repair.h"

#define CG_RS_KMAX CG_REP_SPAN
#define CG_RS_WORK_ENC 64  /* leo_encode_work_count(64, 32) */
#define CG_RS_WORK_DEC 128 /* leo_decode_work_count(64, 32) */
#define CG_RS_RXB 16       /* blocks the receiver follows */
#define CG_RS_RXREP 64     /* repairs it keeps */

/* Leopard's one-time set-up: 0, or -1 (no Leopard in this build, or it
 * refused the platform). Call before any thread uses it. */
int cg_rs_init(void);
/* Whether cg_rs_init succeeded. */
int cg_rs_ready(void);

/* Leopard's symbol size for a block whose longest symbol is slen bytes. */
static inline unsigned cg_rs_bytes(unsigned slen)
{
	return (slen + 63) / 64 * 64;
}

/* Encodes k originals of bytes each into the m repairs work[0..m-1]
 * (work: CG_RS_WORK_ENC buffers); 0, or -1. */
int cg_rs_encode(unsigned k, unsigned m, unsigned bytes, const uint8_t *const *orig, uint8_t **work);
/* Decodes: orig[i] NULL for a missing original, rec[j] NULL for a missing
 * repair; a missing original i comes out in work[i] (work: CG_RS_WORK_DEC
 * buffers); 0, or -1 (too few, or an error). */
int cg_rs_decode(unsigned k, unsigned m, unsigned bytes, const uint8_t *const *orig, const uint8_t *const *rec,
		 uint8_t **work);

/* The sender's open block. */
struct cg_rs_tx {
	uint32_t base;
	uint64_t map;
	uint8_t k, m;     /* members so far, repairs planned */
	uint16_t slen;    /* the longest symbol */
	uint64_t t0_us;   /* when its first member went */
	uint8_t orig[CG_RS_KMAX][CG_RS_BYTES_MAX] __attribute__((aligned(64)));
	uint8_t work[CG_RS_WORK_ENC][CG_RS_BYTES_MAX] __attribute__((aligned(64)));
};

/* Whether DATA seq can join the open block (none open: yes). */
static inline int cg_rs_tx_fits(const struct cg_rs_tx *t, uint32_t seq)
{
	return !t->k || (seq - t->base < CG_REP_SPAN && t->k < CG_RS_KMAX);
}

/* Adds a member (seq, len bytes at p; cg_rs_tx_fits said yes); m: the
 * repairs planned, taken when it opens the block. */
void cg_rs_tx_add(struct cg_rs_tx *t, uint32_t seq, const uint8_t *p, uint16_t len, unsigned m, uint64_t now_us);

/* Closes the block: its repairs' header (h, index 0; the caller sets
 * idx for each) and symbols (sym[j], *bytes each, j < returned count) in
 * the block's work buffers; the count, at most m and k; 0 when nothing
 * was open or Leopard failed. */
unsigned cg_rs_tx_close(struct cg_rs_tx *t, struct cg_rep_hdr *h, uint8_t **sym, unsigned *bytes);

/* The receiver's scratch for a decode: the members it has, padded, and
 * Leopard's work buffers. One per thread, not per session. */
struct cg_rs_scratch {
	uint8_t orig[CG_RS_KMAX][CG_RS_BYTES_MAX] __attribute__((aligned(64)));
	uint8_t work[CG_RS_WORK_DEC][CG_RS_BYTES_MAX] __attribute__((aligned(64)));
};

struct cg_rs_blk {
	uint32_t base;
	uint64_t map;
	uint64_t miss;    /* members missing */
	uint32_t got;     /* repairs kept, bit j */
	uint16_t bytes;
	uint8_t k, m, used;
	uint8_t slot[CG_RS_MMAX]; /* repair j's slot in the pool */
	uint32_t born;
};

struct cg_rs_rx {
	struct cg_rs_blk b[CG_RS_RXB];
	uint64_t pool_used; /* bit i: rep[i] in use */
	uint8_t want[CG_REP_RING];
	uint32_t newest, born;
	uint8_t have_newest;
	uint64_t repairs, useless, recovered, dropped, bad, decodes, failed;
	uint8_t rep[CG_RS_RXREP][CG_RS_BYTES_MAX] __attribute__((aligned(64)));
};

/* A repair (header h, slen bytes of symbol at s): kept while members of
 * its block are missing; once there are as many repairs as missing, the
 * block is decoded into the ring and the rebuilt sequences go into out
 * (nout at most, CG_RS_KMAX is enough); returns how many. */
unsigned cg_rs_rx_repair(struct cg_rs_rx *r, struct cg_rs_scratch *x, struct cg_rep_ring *ring,
			 const struct cg_rep_hdr *h, const uint8_t *s, unsigned slen, uint32_t *out, unsigned nout);

/* DATA seq went into the ring: out of the blocks missing it, and a block
 * that can decode now does (as cg_rs_rx_repair). */
unsigned cg_rs_rx_data(struct cg_rs_rx *r, struct cg_rs_scratch *x, struct cg_rep_ring *ring, uint32_t seq,
		       uint32_t *out, unsigned nout);

#endif
