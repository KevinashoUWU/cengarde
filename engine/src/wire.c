/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wire.h"

#include <string.h>

#include "blake2s.h"

static inline void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The nonce: the direction, three zero bytes, the counter (big-endian). */
static void nonce_of(uint8_t n[CG_AEAD_NONCE_LEN], unsigned dir, uint64_t ctr)
{
	n[0] = (uint8_t)dir;
	n[1] = n[2] = n[3] = 0;
	put32(n + 4, (uint32_t)(ctr >> 32));
	put32(n + 8, (uint32_t)ctr);
}

void cg_wire_key(uint8_t out[CG_AEAD_KEY_LEN], const uint8_t secret[CG_KEY_LEN], unsigned dir, uint32_t session,
		 const uint8_t epoch[CG_EPOCH_LEN])
{
	uint8_t m[16 + 4 + CG_EPOCH_LEN];

	memcpy(m, dir == CG_DIR_C2S ? "cengarde 1.0 c2s" : "cengarde 1.0 s2c", 16);
	put32(m + 16, session);
	memcpy(m + 20, epoch, CG_EPOCH_LEN);
	cg_blake2s(out, CG_AEAD_KEY_LEN, m, sizeof(m), secret, CG_KEY_LEN);
}

void cg_seal_init(struct cg_seal *s, const uint8_t secret[CG_KEY_LEN], unsigned dir, uint32_t session,
		  const uint8_t epoch[CG_EPOCH_LEN])
{
	cg_wire_key(s->key, secret, dir, session, epoch);
	memcpy(s->epoch, epoch, CG_EPOCH_LEN);
	s->ctr = 0;
	s->dir = (uint8_t)dir;
}

void cg_unseal_init(struct cg_unseal *u, const uint8_t secret[CG_KEY_LEN], unsigned dir, uint32_t session,
		    const uint8_t *epoch)
{
	memset(u, 0, sizeof(*u));
	u->dir = (uint8_t)dir;
	if (epoch) {
		cg_wire_key(u->key, secret, dir, session, epoch);
		memcpy(u->epoch, epoch, CG_EPOCH_LEN);
		u->keyed = 1;
	}
}

size_t cg_wire_seal(uint8_t *out, const uint8_t *hdr, const void *payload, size_t plen, struct cg_seal *s,
		    int epoch1)
{
	size_t h = epoch1 ? CG_WIRE_HDR + CG_WIRE_EXT : CG_WIRE_HDR, first;
	uint64_t ctr = s->ctr++;
	uint8_t n[CG_AEAD_NONCE_LEN], ks[64], *ct = out + h;
	const uint8_t *p = payload;

	out[0] = (uint8_t)(CG_PROTO_WIRE << 4 | (epoch1 ? CG_WIRE_KIND_EPOCH : CG_WIRE_KIND_PLAIN));
	out[CG_WIRE_HINT_OFF] = hdr[CG_HINT_OFF];
	out[CG_WIRE_LINK_OFF] = hdr[CG_LINK_OFF];
	memcpy(out + CG_WIRE_SESSION_OFF, hdr + 4, 4);
	put32(out + 7, (uint32_t)ctr);
	if (epoch1) {
		put32(out + CG_WIRE_HDR, (uint32_t)(ctr >> 32));
		memcpy(out + CG_WIRE_HDR + 4, s->epoch, CG_EPOCH_LEN);
	}
	nonce_of(n, s->dir, ctr);
	/* block 1 covers the inner header and the payload's first 50 bytes */
	cg_chacha20_block(ks, s->key, n, 1);
	ct[0] = (uint8_t)((hdr[0] & 0x0f) ^ ks[0]);
	ct[1] = (uint8_t)(hdr[1] ^ ks[1]);
	for (int i = 0; i < 8; i++) /* seq, ts */
		ct[2 + i] = (uint8_t)(hdr[8 + i] ^ ks[2 + i]);
	for (int i = 0; i < 4; i++)
		ct[10 + i] = (uint8_t)(hdr[CG_SSEQ_OFF + i] ^ ks[10 + i]);
	first = plen < 64 - CG_WIRE_INNER ? plen : 64 - CG_WIRE_INNER;
	for (size_t i = 0; i < first; i++)
		ct[CG_WIRE_INNER + i] = (uint8_t)(p[i] ^ ks[CG_WIRE_INNER + i]);
	if (plen > first)
		cg_chacha20(ct + CG_WIRE_INNER + first, p + first, plen - first, s->key, n, 2);
	cg_aead_tag(ct + CG_WIRE_INNER + plen, ct, CG_WIRE_INNER + plen, out, h, s->key, n);
	return h + CG_WIRE_INNER + plen + CG_WIRE_TAG;
}

int cg_wire_peek(struct cg_wire *w, const uint8_t *buf, size_t len)
{
	if (len < CG_WIRE_HDR || buf[0] >> 4 != CG_PROTO_WIRE)
		return -1;
	w->kind = buf[0] & 0x0f;
	if (w->kind > CG_WIRE_KIND_EPOCH)
		return -1;
	w->hlen = w->kind == CG_WIRE_KIND_EPOCH ? CG_WIRE_HDR + CG_WIRE_EXT : CG_WIRE_HDR;
	if (len < w->hlen + CG_WIRE_INNER + CG_WIRE_TAG)
		return -1;
	w->hint = buf[CG_WIRE_HINT_OFF];
	w->link = buf[CG_WIRE_LINK_OFF];
	w->session = get32(buf + CG_WIRE_SESSION_OFF);
	w->nseq = get32(buf + 7);
	w->nhigh = w->kind == CG_WIRE_KIND_EPOCH ? get32(buf + CG_WIRE_HDR) : 0;
	if (w->kind == CG_WIRE_KIND_EPOCH)
		memcpy(w->epoch, buf + CG_WIRE_HDR + 4, CG_EPOCH_LEN);
	return 0;
}

uint64_t cg_wire_ctr(const struct cg_wire *w, uint64_t top)
{
	uint64_t c;

	if (w->kind == CG_WIRE_KIND_EPOCH)
		return (uint64_t)w->nhigh << 32 | w->nseq;
	c = (top & ~(uint64_t)0xffffffff) | w->nseq;
	if (c + 0x80000000ull < top)
		c += 1ull << 32;
	else if (c > top + 0x80000000ull && c >= 1ull << 32)
		c -= 1ull << 32;
	return c;
}

int cg_wire_open_head(struct cg_hdr *h, const uint8_t *buf, size_t len, const struct cg_wire *w,
		      const uint8_t key[CG_AEAD_KEY_LEN], unsigned dir, uint64_t ctr)
{
	uint8_t n[CG_AEAD_NONCE_LEN], want[CG_WIRE_TAG], in[CG_WIRE_INNER];
	const uint8_t *ct = buf + w->hlen;
	size_t clen = len - w->hlen - CG_WIRE_TAG;

	nonce_of(n, dir, ctr);
	cg_aead_tag(want, ct, clen, buf, w->hlen, key, n);
	if (!cg_aead_tag_eq(want, buf + len - CG_WIRE_TAG))
		return -1;
	cg_chacha20_at(in, ct, CG_WIRE_INNER, key, n, 0);
	if (in[0] > 0x0f)
		return -1;
	h->type = in[0];
	h->flags = in[1];
	h->hint = w->hint;
	h->link = w->link;
	h->session = w->session;
	h->seq = get32(in + 2);
	h->ts = get32(in + 6);
	h->sseq = get32(in + 10);
	return 0;
}

size_t cg_wire_open_body(uint8_t *buf, size_t len, const struct cg_hdr *h, const struct cg_wire *w,
			 const uint8_t key[CG_AEAD_KEY_LEN], unsigned dir, uint64_t ctr)
{
	uint8_t n[CG_AEAD_NONCE_LEN];
	uint8_t *p = buf + w->hlen + CG_WIRE_INNER;
	size_t plen = len - w->hlen - CG_WIRE_INNER - CG_WIRE_TAG;

	nonce_of(n, dir, ctr);
	cg_chacha20_at(p, p, plen, key, n, CG_WIRE_INNER);
	if (!cg_hdr_plen_ok(h->type, p, plen))
		return 0;
	memmove(buf + CG_HDR_LEN, p, plen);
	cg_hdr_put(buf, h);
	return CG_HDR_LEN + plen;
}

const uint8_t *cg_unseal_key(const struct cg_unseal *u, const struct cg_wire *w, const uint8_t secret[CG_KEY_LEN],
			     uint32_t session, uint8_t tmp[CG_AEAD_KEY_LEN])
{
	if (w->kind == CG_WIRE_KIND_EPOCH && (!u->keyed || memcmp(w->epoch, u->epoch, CG_EPOCH_LEN))) {
		cg_wire_key(tmp, secret, u->dir, session, w->epoch);
		return tmp;
	}
	return u->keyed ? u->key : NULL;
}

void cg_unseal_accept(struct cg_unseal *u, const struct cg_wire *w, const uint8_t *key, uint64_t ctr)
{
	if (w->kind == CG_WIRE_KIND_EPOCH && (!u->keyed || memcmp(w->epoch, u->epoch, CG_EPOCH_LEN))) {
		memcpy(u->key, key, CG_AEAD_KEY_LEN);
		memcpy(u->epoch, w->epoch, CG_EPOCH_LEN);
		u->keyed = 1;
		u->top = ctr;
		u->seen = 1;
		return;
	}
	if (!u->seen || ctr > u->top) {
		u->top = ctr;
		u->seen = 1;
	}
}
