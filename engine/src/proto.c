/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "proto.h"

#include <string.h>

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

static inline void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)(v >> 32));
	put32(p + 4, (uint32_t)v);
}

static inline uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) << 32 | get32(p + 4);
}

static uint64_t mac(const uint8_t *hdr, const uint8_t key[CG_SIPHASH_KEY_LEN], const void *payload,
		    size_t plen)
{
	struct cg_siphash s;
	uint8_t canon[CG_MAC_OFF];

	memcpy(canon, hdr, CG_MAC_OFF);
	canon[CG_LINK_OFF] = 0;
	cg_siphash_init(&s, key);
	cg_siphash_update(&s, canon, sizeof(canon));
	cg_siphash_update(&s, payload, plen);
	return cg_siphash_final(&s);
}

void cg_hdr_write(uint8_t out[CG_HDR_LEN], const struct cg_hdr *h, const uint8_t key[CG_SIPHASH_KEY_LEN],
		  const void *payload, size_t plen)
{
	uint64_t m;

	out[0] = (uint8_t)(CG_PROTO_VERSION << 4 | (h->type & 0x0f));
	out[1] = h->flags;
	out[CG_HINT_OFF] = h->hint;
	out[CG_LINK_OFF] = h->link;
	put32(out + 4, h->session);
	put32(out + 8, h->seq);
	put32(out + 12, h->ts);
	m = mac(out, key, payload, plen);
	for (int i = 0; i < 8; i++)
		out[CG_MAC_OFF + i] = (uint8_t)(m >> (8 * i));
}

int cg_hdr_parse(struct cg_hdr *h, const uint8_t *buf, size_t len)
{
	size_t plen;

	if (len < CG_HDR_LEN || buf[0] >> 4 != CG_PROTO_VERSION)
		return -1;
	h->type = buf[0] & 0x0f;
	h->flags = buf[1];
	h->hint = buf[CG_HINT_OFF];
	h->link = buf[CG_LINK_OFF];
	h->session = get32(buf + 4);
	h->seq = get32(buf + 8);
	h->ts = get32(buf + 12);
	plen = len - CG_HDR_LEN;
	switch (h->type) {
	case CG_T_DATA:
		return plen > 0 ? 0 : -1;
	case CG_T_PROBE:
	case CG_T_PROBE_REPLY:
		return plen >= CG_PROBE_INFO_LEN ? 0 : -1;
	case CG_T_HELLO:
		return plen >= CG_HELLO_LEN ? 0 : -1;
	case CG_T_NACK:
		return plen >= 1 && buf[CG_HDR_LEN] >= 1 && buf[CG_HDR_LEN] <= CG_NACK_RANGES &&
				       plen >= 1 + 12 * (size_t)buf[CG_HDR_LEN]
			       ? 0
			       : -1;
	case CG_T_FEC:
		/* base, count, lengths' XOR and one byte at least (fec.h) */
		return plen >= 8 ? 0 : -1;
	default:
		return -1;
	}
}

int cg_hdr_verify(const uint8_t *buf, size_t len, const uint8_t key[CG_SIPHASH_KEY_LEN])
{
	uint64_t m;
	uint8_t diff = 0;

	if (len < CG_HDR_LEN)
		return 0;
	m = mac(buf, key, buf + CG_HDR_LEN, len - CG_HDR_LEN);
	for (int i = 0; i < 8; i++)
		diff |= buf[CG_MAC_OFF + i] ^ (uint8_t)(m >> (8 * i));
	return diff == 0;
}

void cg_probe_info_write(uint8_t out[CG_PROBE_INFO_LEN], const struct cg_probe_info *pi)
{
	put32(out, pi->echo_ts);
	put32(out + 4, pi->owd);
	put32(out + 8, pi->interval_ms);
	put32(out + 12, pi->rx);
	put32(out + 16, pi->wins);
	put32(out + 20, pi->lag_us);
	put32(out + 24, pi->cookie);
	put32(out + 28, pi->rx_top);
	put32(out + 32, pi->rx_top_ctl);
	put32(out + 36, pi->tx_next);
}

int cg_looks_like_wg(const uint8_t *buf, size_t len)
{
	if (len < 32 || buf[1] || buf[2] || buf[3])
		return 0;
	switch (buf[0]) {
	case 1:
		return len == 148;
	case 2:
		return len == 92;
	case 3:
		return len == 64;
	case 4:
		return 1;
	default:
		return 0;
	}
}

void cg_probe_info_read(struct cg_probe_info *pi, const uint8_t in[CG_PROBE_INFO_LEN])
{
	pi->echo_ts = get32(in);
	pi->owd = get32(in + 4);
	pi->interval_ms = get32(in + 8);
	pi->rx = get32(in + 12);
	pi->wins = get32(in + 16);
	pi->lag_us = get32(in + 20);
	pi->cookie = get32(in + 24);
	pi->rx_top = get32(in + 28);
	pi->rx_top_ctl = get32(in + 32);
	pi->tx_next = get32(in + 36);
}

void cg_probe_bond_write(uint8_t *payload, const struct cg_probe_bond *b)
{
	uint32_t w = b->weight > CG_BOND_WEIGHT_MAX ? CG_BOND_WEIGHT_MAX : b->weight;

	put32(payload + CG_PROBE_INFO_LEN, w | (b->reorders ? CG_BOND_REORDERS : 0) |
						   (b->split_me ? CG_BOND_SPLIT_ME : 0) | (b->rescue ? CG_BOND_RESCUE : 0) |
						   (b->full ? CG_BOND_FULL : 0));
}

int cg_probe_bond_read(struct cg_probe_bond *b, const uint8_t *payload, size_t plen)
{
	uint32_t v, w;

	memset(b, 0, sizeof(*b));
	if (plen < CG_PROBE_BOND_LEN)
		return 0;
	v = get32(payload + CG_PROBE_INFO_LEN);
	w = v & CG_BOND_WEIGHT;
	b->weight = (uint16_t)(w > CG_BOND_WEIGHT_MAX ? CG_BOND_WEIGHT_MAX : w);
	b->reorders = !!(v & CG_BOND_REORDERS);
	b->split_me = !!(v & CG_BOND_SPLIT_ME);
	b->rescue = !!(v & CG_BOND_RESCUE);
	b->full = !!(v & CG_BOND_FULL);
	return 1;
}

size_t cg_nack_write(uint8_t *out, const uint32_t *seq, unsigned n, unsigned *used)
{
	unsigned k = 0, i;
	uint32_t base = 0;
	uint64_t mask = 0;

	for (i = 0; i < n; i++) {
		uint32_t d = seq[i] - base;

		if (k && d >= 1 && d <= 64) {
			mask |= 1ULL << (d - 1);
			continue;
		}
		if (k) {
			put32(out + 1 + 12 * (k - 1), base);
			put64(out + 5 + 12 * (k - 1), mask);
		}
		if (k == CG_NACK_RANGES)
			break;
		base = seq[i];
		mask = 0;
		k++;
	}
	if (i == n && k) {
		put32(out + 1 + 12 * (k - 1), base);
		put64(out + 5 + 12 * (k - 1), mask);
	}
	*used = i;
	out[0] = (uint8_t)k;
	return 1 + 12 * (size_t)k;
}

unsigned cg_nack_read(uint32_t *seq, unsigned max, const uint8_t *payload, size_t plen)
{
	unsigned k = payload[0], n = 0;

	if (k > CG_NACK_RANGES || plen < 1 + 12 * (size_t)k)
		return 0;
	for (unsigned r = 0; r < k; r++) {
		uint32_t base = get32(payload + 1 + 12 * r);
		uint64_t mask = get64(payload + 5 + 12 * r);

		if (n < max)
			seq[n++] = base;
		for (unsigned b = 0; b < 64 && n < max; b++)
			if (mask >> b & 1)
				seq[n++] = base + 1 + b;
	}
	return n;
}

void cg_hello_write(uint8_t out[CG_HELLO_LEN], const struct cg_hello *hl)
{
	put32(out, hl->echo_ts);
	put32(out + 4, hl->cookie);
}

void cg_hello_read(struct cg_hello *hl, const uint8_t in[CG_HELLO_LEN])
{
	hl->echo_ts = get32(in);
	hl->cookie = get32(in + 4);
}
