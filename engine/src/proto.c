/* SPDX-License-Identifier: GPL-2.0-only */
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
	out[2] = 0;
	out[3] = h->link;
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

	if (len < CG_HDR_LEN || buf[0] >> 4 != CG_PROTO_VERSION || buf[2] != 0)
		return -1;
	h->type = buf[0] & 0x0f;
	h->flags = buf[1];
	h->link = buf[3];
	h->session = get32(buf + 4);
	h->seq = get32(buf + 8);
	h->ts = get32(buf + 12);
	plen = len - CG_HDR_LEN;
	switch (h->type) {
	case CG_T_DATA:
		return plen > 0 ? 0 : -1;
	case CG_T_PROBE:
	case CG_T_PROBE_REPLY:
		return plen == CG_PROBE_INFO_LEN ? 0 : -1;
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
}
