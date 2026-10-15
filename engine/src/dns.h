/* DNS messages for the server names: the query the client sends and what an
 * answer says about it.
 *
 * The client resolves the names in "server" itself, through each link,
 * never through the system resolver, whose traffic may go through the very
 * tunnel that is down once the server moves. This is only the codec: one
 * question (A or AAAA, class IN, RD set, no EDNS) and the reading of what
 * comes back, which is untrusted input from the network:
 *
 * - Every read is bounds-checked. A name is read through compression
 *   pointers, each pointing after the header and strictly before where the
 *   labels holding it began (so a pointer never goes forward, and no loop
 *   is possible), at most CG_DNS_MAX_JUMPS of them, the name at most
 *   CG_DNS_WIRE_MAX octets in wire form. Label types 0x40 and 0x80 are
 *   malformed. Names compare case-insensitively (ASCII).
 * - An answer belongs to the query by its id and must echo the question;
 *   the error rcodes are taken as they come, also without a question (some
 *   servers send REFUSED that way).
 * - The answer section is walked strictly: every record must fit, those of
 *   other names, types or classes are skipped. CNAMEs are followed from the
 *   name, in whatever order they come, at most CG_DNS_MAX_HOPS of them; a
 *   loop never ends, so it runs into that limit. The address records of the
 *   name the chain ends at are kept, CG_DNS_MAX_ADDRS at most, each once.
 * - A truncated answer (TC) counts with the records that arrived whole.
 * - The authority and additional sections are never read: no glue, no SOA.
 *
 * The work grows with the hops times the message's length: the caller reads
 * into a buffer of a sensible size (no EDNS, so a server sends 512 octets).
 *
 * Pure functions, no clock and no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_DNS_H
#define CG_DNS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CG_DNS_NAME_MAX 253  /* a name, without a trailing dot */
#define CG_DNS_A 1
#define CG_DNS_AAAA 28
#define CG_DNS_MAX_ADDRS 16  /* addresses kept from one answer */
#define CG_DNS_QUERY_MAX (12 + CG_DNS_NAME_MAX + 2 + 4)

#define CG_DNS_HDR 12        /* header octets */
#define CG_DNS_WIRE_MAX 255  /* a name in wire form, with its root label */
#define CG_DNS_MAX_HOPS 8    /* CNAMEs followed */
#define CG_DNS_MAX_JUMPS 128 /* compression pointers in one name */
#define CG_DNS_CNAME 5
#define CG_DNS_IN 1

enum cg_dns_outcome {
	CG_DNS_IGNORE,    /* not an answer to this query: shorter than a header, another id, or not a response (QR 0) */
	CG_DNS_ADDRS,     /* addresses of qtype for the name (after following CNAMEs) */
	CG_DNS_NODATA,    /* NOERROR without any address of qtype for the name */
	CG_DNS_NXDOMAIN,  /* rcode 3 */
	CG_DNS_SERVFAIL,  /* rcode 2, and any other error rcode but 3 and 5 */
	CG_DNS_REFUSED,   /* rcode 5 */
	CG_DNS_MALFORMED, /* the right id, but not a well-formed answer to this question */
};

struct cg_dns_result {
	int outcome;     /* enum cg_dns_outcome */
	uint8_t rcode;
	uint8_t tc;      /* the TC bit */
	uint32_t ttl;    /* CG_DNS_ADDRS: the smallest TTL along the chain (CNAMEs followed and addresses kept) */
	int n;           /* addresses; 0 unless CG_DNS_ADDRS */
	uint8_t addr[CG_DNS_MAX_ADDRS][16]; /* A: the first 4 bytes */
};

/* Normalizes a host name as written in a configuration: lowercase, without a
 * trailing dot. 1..253 characters once normalized, labels of 1..63 characters
 * of [a-z0-9_-] (no empty label: no leading dot, no "a..b"), and the last
 * label not all digits (a mistyped IPv4 address such as 10.0.0.256 is no
 * name). Returns 0, or -1 when it is not a valid name (out untouched or
 * partially written, the caller ignores it). outlen >= CG_DNS_NAME_MAX + 1
 * always fits. */
static inline int cg_dns_name_norm(const char *in, char *out, size_t outlen)
{
	size_t n = strnlen(in, CG_DNS_NAME_MAX + 2), label = 0;
	int digits = 1; /* the label so far is all digits */

	if (n && in[n - 1] == '.')
		n--;
	if (n < 1 || n > CG_DNS_NAME_MAX || outlen <= n)
		return -1;
	for (size_t i = 0; i < n; i++) {
		char c = in[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c == '.') {
			if (!label)
				return -1;
			label = 0;
			digits = 1;
		} else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
			if (++label > 63)
				return -1;
			digits &= c >= '0' && c <= '9';
		} else {
			return -1;
		}
		out[i] = c;
	}
	if (!label || digits)
		return -1;
	out[n] = 0;
	return 0;
}

/* The wire form of name into out (CG_DNS_WIRE_MAX octets): its length, or -1
 * when name is not a valid name as cg_dns_name_norm leaves it. */
static inline int cg_dns_name_wire(const char *name, uint8_t *out)
{
	char norm[CG_DNS_NAME_MAX + 1];
	size_t n;

	if (cg_dns_name_norm(name, norm, sizeof(norm)) || strcmp(norm, name))
		return -1;
	n = strlen(name);
	/* The label starting at name[s] has its length at out[s] and name[j] at
	 * out[j + 1]: a dot becomes the length of the label after it. */
	for (size_t i = 0, s = 0; i <= n; i++) {
		if (i == n || name[i] == '.') {
			out[s] = (uint8_t)(i - s);
			s = i + 1;
		} else {
			out[i + 1] = (uint8_t)name[i];
		}
	}
	out[n + 1] = 0;
	return (int)n + 2;
}

static inline uint16_t cg_dns_get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t cg_dns_get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void cg_dns_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* A query for a normalized name: header (id, RD set, QDCOUNT 1, nothing
 * else, no EDNS), the name, qtype, class IN. Returns its length, or -1 when
 * it does not fit cap, the name is not valid or qtype is neither A nor
 * AAAA. */
static inline int cg_dns_query(uint8_t *buf, size_t cap, uint16_t id, const char *name, uint16_t qtype)
{
	uint8_t w[CG_DNS_WIRE_MAX];
	int wn = cg_dns_name_wire(name, w);
	size_t n;

	if (wn < 0 || (qtype != CG_DNS_A && qtype != CG_DNS_AAAA))
		return -1;
	n = CG_DNS_HDR + (size_t)wn + 4;
	if (n > cap)
		return -1;
	memset(buf, 0, CG_DNS_HDR);
	cg_dns_put16(buf, id);
	buf[2] = 0x01; /* RD */
	buf[5] = 1;    /* QDCOUNT */
	memcpy(buf + CG_DNS_HDR, w, (size_t)wn);
	cg_dns_put16(buf + CG_DNS_HDR + wn, qtype);
	cg_dns_put16(buf + CG_DNS_HDR + wn + 2, CG_DNS_IN);
	return (int)n;
}

enum {
	CG_DNS_CUT = -1, /* the message ends inside it: truncated */
	CG_DNS_BAD = -2, /* malformed */
};

/* Reads the name at *off of the len octets of msg into out (CG_DNS_WIRE_MAX
 * octets), in wire form, uncompressed and lowercase, and moves *off past it.
 * The labels in place must end by end (a record's rdata end, or len), those
 * a pointer leads to by len. Returns its length in out, CG_DNS_CUT when the
 * labels in place run into the end of the message (end == len), or
 * CG_DNS_BAD. */
static inline int cg_dns_name_read(const uint8_t *msg, size_t len, size_t *off, size_t end, uint8_t *out)
{
	size_t p = *off, seg = p, after = 0; /* seg: where these labels began; after: past the first pointer */
	int n = 0, jumps = 0;

	for (;;) {
		size_t lim = after ? len : end;
		uint8_t c;

		if (p >= lim)
			break;
		c = msg[p];
		if (c >= 0xc0) {
			size_t to;

			if (lim - p < 2)
				break;
			to = (size_t)(c & 0x3f) << 8 | msg[p + 1];
			if (to < CG_DNS_HDR || to >= seg || ++jumps > CG_DNS_MAX_JUMPS)
				return CG_DNS_BAD;
			if (!after)
				after = p + 2;
			p = seg = to;
			continue;
		}
		if (c & 0xc0 || n + 1 + c > CG_DNS_WIRE_MAX)
			return CG_DNS_BAD;
		if (lim - p < 1 + (size_t)c)
			break;
		out[n++] = c;
		for (size_t i = 1; i <= c; i++) {
			uint8_t b = msg[p + i];

			out[n++] = b >= 'A' && b <= 'Z' ? (uint8_t)(b + ('a' - 'A')) : b;
		}
		p += 1 + (size_t)c;
		if (!c) {
			*off = after ? after : p;
			return n;
		}
	}
	return !after && end == len ? CG_DNS_CUT : CG_DNS_BAD;
}

/* A resource record as cg_dns_rr_read finds it. */
struct cg_dns_rr {
	uint16_t type, cls;
	uint32_t ttl;
	size_t rdata, rdlen; /* offset and length of its data */
};

/* Reads the record at *off into rr and its owner into owner (CG_DNS_WIRE_MAX
 * octets), and moves *off past it. Returns the owner's length, CG_DNS_CUT or
 * CG_DNS_BAD. */
static inline int cg_dns_rr_read(const uint8_t *msg, size_t len, size_t *off, struct cg_dns_rr *rr,
				 uint8_t *owner)
{
	size_t p = *off;
	int n = cg_dns_name_read(msg, len, &p, len, owner);

	if (n < 0)
		return n;
	if (len - p < 10)
		return CG_DNS_CUT;
	rr->type = cg_dns_get16(msg + p);
	rr->cls = cg_dns_get16(msg + p + 2);
	rr->ttl = cg_dns_get32(msg + p + 4);
	rr->rdlen = cg_dns_get16(msg + p + 8);
	rr->rdata = p + 10;
	if (len - rr->rdata < rr->rdlen)
		return CG_DNS_CUT;
	*off = rr->rdata + rr->rdlen;
	return n;
}

static inline int cg_dns_same(const uint8_t *a, int an, const uint8_t *b, int bn)
{
	return an == bn && !memcmp(a, b, (size_t)an);
}

/* A TTL with the high bit set counts as 0 (RFC 2181, 8). */
static inline uint32_t cg_dns_ttl_min(uint32_t ttl, uint32_t rr_ttl)
{
	if (rr_ttl & 0x80000000u)
		rr_ttl = 0;
	return rr_ttl < ttl ? rr_ttl : ttl;
}

/* The outcome of an error rcode (not 0). */
static inline int cg_dns_rcode_outcome(uint8_t rcode)
{
	return rcode == 3 ? CG_DNS_NXDOMAIN : rcode == 5 ? CG_DNS_REFUSED : CG_DNS_SERVFAIL;
}

/* What a datagram says about the query (id, normalized name, qtype A or
 * AAAA; with any other qtype, or a name not valid, no answer matches). */
static inline void cg_dns_parse(const uint8_t *msg, size_t len, uint16_t id, const char *name, uint16_t qtype,
				struct cg_dns_result *r)
{
	uint8_t want[CG_DNS_WIRE_MAX], got[CG_DNS_WIRE_MAX];
	size_t alen = qtype == CG_DNS_A ? 4 : qtype == CG_DNS_AAAA ? 16 : 0, off = CG_DNS_HDR, ans;
	uint32_t ttl = UINT32_MAX;
	uint16_t qd, an;
	int wn, gn, nrec, hops = 0;
	struct cg_dns_rr rr;

	memset(r, 0, sizeof(*r));
	if (len < CG_DNS_HDR || cg_dns_get16(msg) != id || !(msg[2] & 0x80))
		return; /* CG_DNS_IGNORE */
	r->outcome = CG_DNS_MALFORMED;
	r->rcode = msg[3] & 0x0f;
	r->tc = msg[2] >> 1 & 1;
	qd = cg_dns_get16(msg + 4);
	an = cg_dns_get16(msg + 6);
	if (msg[2] >> 3 & 0x0f) /* opcode */
		return;
	if (!qd && r->rcode) {
		r->outcome = cg_dns_rcode_outcome(r->rcode);
		return;
	}

	/* The question, whole even when truncated. */
	wn = cg_dns_name_wire(name, want);
	if (qd != 1 || !alen || wn < 0)
		return;
	gn = cg_dns_name_read(msg, len, &off, len, got);
	if (!cg_dns_same(got, gn, want, wn) || len - off < 4 || cg_dns_get16(msg + off) != qtype ||
	    cg_dns_get16(msg + off + 2) != CG_DNS_IN)
		return;
	off += 4;
	if (r->rcode) {
		r->outcome = cg_dns_rcode_outcome(r->rcode);
		return;
	}

	/* Every record of the answer section must fit, but with TC those
	 * before the cut count. */
	ans = off;
	for (nrec = 0; nrec < an; nrec++) {
		gn = cg_dns_rr_read(msg, len, &off, &rr, got);
		if (gn == CG_DNS_CUT && r->tc)
			break;
		if (gn < 0)
			return;
	}

	/* The CNAME chain from the name, in any order: want is the target, and
	 * each hop scans the section again for the new one. */
	for (int moved = 1; moved;) {
		moved = 0;
		off = ans;
		for (int i = 0; i < nrec && !moved; i++) {
			size_t p, end;

			gn = cg_dns_rr_read(msg, len, &off, &rr, got);
			if (gn < 0)
				return;
			if (rr.type != CG_DNS_CNAME || rr.cls != CG_DNS_IN || !cg_dns_same(got, gn, want, wn))
				continue;
			if (++hops > CG_DNS_MAX_HOPS)
				return;
			p = rr.rdata;
			end = rr.rdata + rr.rdlen;
			wn = cg_dns_name_read(msg, len, &p, end, want);
			if (wn < 0 || p != end) /* the name fills its rdata */
				return;
			ttl = cg_dns_ttl_min(ttl, rr.ttl);
			moved = 1;
		}
	}

	/* The addresses of the target. */
	off = ans;
	for (int i = 0; i < nrec; i++) {
		int k = 0;

		gn = cg_dns_rr_read(msg, len, &off, &rr, got);
		if (gn < 0)
			goto malformed;
		if (rr.type != qtype || rr.cls != CG_DNS_IN || !cg_dns_same(got, gn, want, wn))
			continue;
		if (rr.rdlen != alen)
			goto malformed;
		while (k < r->n && memcmp(r->addr[k], msg + rr.rdata, alen))
			k++;
		if (k == CG_DNS_MAX_ADDRS)
			continue; /* full: ignored */
		ttl = cg_dns_ttl_min(ttl, rr.ttl);
		if (k == r->n)
			memcpy(r->addr[r->n++], msg + rr.rdata, alen);
	}
	if (r->n) {
		r->outcome = CG_DNS_ADDRS;
		r->ttl = ttl;
	} else if (!r->tc) {
		r->outcome = CG_DNS_NODATA;
	}
	return;

malformed:
	r->n = 0;
	memset(r->addr, 0, sizeof(r->addr));
}

/* A short name for an outcome, for the status: "", "nodata", "nxdomain",
 * "servfail", "refused", "malformed" (IGNORE and ADDRS give ""). */
static inline const char *cg_dns_outcome_name(int outcome)
{
	switch (outcome) {
	case CG_DNS_NODATA:
		return "nodata";
	case CG_DNS_NXDOMAIN:
		return "nxdomain";
	case CG_DNS_SERVFAIL:
		return "servfail";
	case CG_DNS_REFUSED:
		return "refused";
	case CG_DNS_MALFORMED:
		return "malformed";
	default:
		return "";
	}
}

#endif
