/* zstd on the reliable lane (story 031, step 3d): one compression context
 * a datagram stream and way, in streaming mode, flushed at the end of each
 * datagram. Its history (the last CG_ZL_WLOG bits' worth of bytes) is every
 * datagram before: a burst of JSON telemetry, the same keys and shapes
 * again, shrinks fast, and with no dictionary to agree on, since the lane
 * is ordered and reliable, so the other end's decompressor has seen the
 * same datagrams in the same order (the design of 2026-11-01's DC-SYN and
 * DC-ACK is for what is neither).
 *
 * Built with libzstd only (CG_ZSTD); the rest of the engine never needs
 * it. Pure: no clock, no I/O; the contexts are the caller's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_ZLANE_H
#define CG_ZLANE_H

#ifdef CG_ZSTD

#include <stddef.h>
#include <stdint.h>

#include <zstd.h>

#define CG_ZL_WLOG 17  /* 128 KB of history a way */
#define CG_ZL_LEVEL 3

/* A compressor for one way of a stream; NULL without memory. */
static inline ZSTD_CCtx *cg_zl_cnew(void)
{
	ZSTD_CCtx *z = ZSTD_createCCtx();

	if (z && (ZSTD_isError(ZSTD_CCtx_setParameter(z, ZSTD_c_compressionLevel, CG_ZL_LEVEL)) ||
		  ZSTD_isError(ZSTD_CCtx_setParameter(z, ZSTD_c_windowLog, CG_ZL_WLOG)) ||
		  ZSTD_isError(ZSTD_CCtx_setParameter(z, ZSTD_c_checksumFlag, 0)))) {
		ZSTD_freeCCtx(z);
		return NULL;
	}
	return z;
}

static inline ZSTD_DCtx *cg_zl_dnew(void)
{
	ZSTD_DCtx *z = ZSTD_createDCtx();

	if (z && ZSTD_isError(ZSTD_DCtx_setParameter(z, ZSTD_d_windowLogMax, CG_ZL_WLOG))) {
		ZSTD_freeDCtx(z);
		return NULL;
	}
	return z;
}

/* Datagram d (n bytes) compressed into out (cap bytes), on z's history:
 * its length (0 for an empty one: nothing new to say), or (size_t)-1 when
 * it does not fit or zstd failed (the context is then of no more use: the
 * stream ends). */
static inline size_t cg_zl_comp(ZSTD_CCtx *z, const uint8_t *d, size_t n, uint8_t *out, size_t cap)
{
	ZSTD_inBuffer in = { d, n, 0 };
	ZSTD_outBuffer o = { out, cap, 0 };
	size_t r;

	do {
		r = ZSTD_compressStream2(z, &o, &in, ZSTD_e_flush);
		if (ZSTD_isError(r) || (r && o.pos == o.size))
			return (size_t)-1;
	} while (r);
	return o.pos;
}

/* A compressed datagram (n bytes at c) back into out (cap bytes), on z's
 * history: its length, or (size_t)-1 when it is not one. */
static inline size_t cg_zl_decomp(ZSTD_DCtx *z, const uint8_t *c, size_t n, uint8_t *out, size_t cap)
{
	ZSTD_inBuffer in = { c, n, 0 };
	ZSTD_outBuffer o = { out, cap, 0 };

	while (in.pos < in.size) {
		size_t r = ZSTD_decompressStream(z, &o, &in);

		if (ZSTD_isError(r) || (o.pos == o.size && in.pos < in.size))
			return (size_t)-1;
	}
	return o.pos;
}

#endif /* CG_ZSTD */
#endif
