/* The forward table: which public ports of a server go to which router.
 *
 * A server (a VPS) serves several routers, each a [client NAME] section. It
 * can hand its public ports to them in two ways:
 *
 * - IP pass: the whole range 1024 to 65000 to ONE router, the one that asks
 *   for it in its probes (and whose section allows it: passthrough = yes).
 * - Explicit rules, "forward = ..." in a [client] section: a port or a range
 *   of tcp or udp ports, optionally to other ports on the router.
 *
 * The engine does not touch the firewall. It writes the outcome as a text
 * file, the forward table, and the script that owns the firewall
 * (contrib/vps/cengarde-nat) reads it and applies it. This header holds the
 * decisions in between, all of them pure:
 *
 * - cg_fwd_parse reads a "forward" list when the configuration is loaded, so
 *   that a typo is refused where the operator sees it. The script would only
 *   skip the bad line of the table and log its number, and the port would
 *   stay closed with nothing wrong on the engine's side.
 * - cg_fwd_overlap finds two rules that take the same port of the same
 *   protocol, which the script resolves by dropping the later one: the engine
 *   names that rule at load time instead. A rule may overlap the IP pass
 *   range, which is not a clash: the script applies the rules first and the
 *   range takes what is left, so a rule carves itself out of it.
 * - cg_pass_holder elects who holds the IP pass range. It is sticky: a router
 *   that has the range keeps it for as long as it keeps asking, however many
 *   others ask after it, so the range never jumps from one router to another
 *   (every connection through the old one would break for nothing). It also
 *   keeps it while the router's wish is not known, which is how the server
 *   looks after a restart, before any probe has told it again.
 * - cg_fwd_table writes the text and cg_fwd_table_holder reads the holder
 *   back from the table written before the engine stopped, so that after a
 *   restart the range stays with its router until that router says
 *   otherwise.
 *
 * The table is data only: a header line naming the format version, then
 *
 *	pass NAME
 *	rule NAME tcp|udp FIRST LAST TO
 *
 * at most one "pass" line, and a rule sends ports FIRST to LAST of the
 * protocol to the router NAME, FIRST arriving on port TO (a range keeps its
 * offsets). The script reads it as data, never as a program, and checks every
 * line again; a name is a router name (lowercase letters, digits and
 * hyphens), which the caller has validated. Every line ends with "\n".
 *
 * A "forward" list is PROTO:PORT[-PORT][=TO] entries, with PROTO tcp, udp or
 * both, a port a decimal number from 1 to 65535, and TO (the port FIRST goes
 * to on the router) FIRST when omitted. "both" is a tcp rule and a udp rule.
 *
 * No allocation, no I/O: the caller brings the state, the clock and the
 * buffers.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_FWDTABLE_H
#define CG_FWDTABLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CG_FWD_MAX 1024 /* rules in a configuration (a "both" entry counts as two) */
#define CG_FWD_SHOWN 40 /* characters of a bad entry an error message quotes */

/* The first line of a table, and the version it carries. The script refuses
 * a table of another version, so a format change bumps the number. */
#define CG_FWD_VERSION "1"
#define CG_FWD_TITLE "# cengarde forward table "
#define CG_FWD_HEADER CG_FWD_TITLE CG_FWD_VERSION ", written by the engine: data only\n"

enum { CG_FWD_TCP = 1, CG_FWD_UDP = 2 };

struct cg_fwd_rule {
	uint16_t first, last; /* the server's ports, first <= last */
	uint16_t to;          /* where first goes on the router */
	uint8_t proto;        /* CG_FWD_TCP or CG_FWD_UDP */
	uint8_t client;       /* index of its [client] section */
};

/* A port in s[0..len): 1 to 5 decimal digits, nothing else (no sign, no
 * blank), 1 to 65535. Returns it, or -1. */
static inline int cg_fwd_port(const char *s, size_t len)
{
	int v = 0;

	if (!len || len > 5)
		return -1;
	for (size_t i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (s[i] - '0');
	}
	return v >= 1 && v <= 65535 ? v : -1;
}

/* Entries are separated by spaces, tabs or commas, in any number. */
static inline int cg_fwd_sep(char c)
{
	return c == ' ' || c == '\t' || c == ',';
}

/* "'ENTRY': why" into err (the entry cut at CG_FWD_SHOWN characters, then
 * "..."). */
static inline void cg_fwd_bad(char *err, size_t errlen, const char *tok, size_t len, const char *why)
{
	if (!err || !errlen)
		return;
	if (len > CG_FWD_SHOWN)
		snprintf(err, errlen, "'%.*s...': %s", CG_FWD_SHOWN, tok, why);
	else
		snprintf(err, errlen, "'%.*s': %s", (int)len, tok, why);
}

/* One entry, tok[0..len), into r: one rule, or two for "both" (tcp first).
 * Returns how many, or 0 with err set. The protocol is checked first, then
 * the ports one by one, then how they fit together. */
static inline int cg_fwd_entry(const char *tok, size_t len, uint8_t client, struct cg_fwd_rule r[2], char *err,
			       size_t errlen)
{
	const char *colon = memchr(tok, ':', len), *rest, *eq, *dash;
	size_t plen, rlen, mlen;
	int first, last, to, k = 1;

	if (!colon) {
		cg_fwd_bad(err, errlen, tok, len, "expected PROTO:PORT[-PORT][=TO], as in tcp:9000=22");
		return 0;
	}
	plen = (size_t)(colon - tok);
	if (plen == 3 && !memcmp(tok, "tcp", 3)) {
		r[0].proto = CG_FWD_TCP;
	} else if (plen == 3 && !memcmp(tok, "udp", 3)) {
		r[0].proto = CG_FWD_UDP;
	} else if (plen == 4 && !memcmp(tok, "both", 4)) {
		r[0].proto = CG_FWD_TCP;
		k = 2;
	} else {
		cg_fwd_bad(err, errlen, tok, len, "expected tcp, udp or both");
		return 0;
	}

	rest = colon + 1;
	rlen = len - plen - 1;
	eq = memchr(rest, '=', rlen);
	mlen = eq ? (size_t)(eq - rest) : rlen;
	dash = memchr(rest, '-', mlen);
	first = cg_fwd_port(rest, dash ? (size_t)(dash - rest) : mlen);
	last = dash ? cg_fwd_port(dash + 1, mlen - (size_t)(dash - rest) - 1) : first;
	to = eq ? cg_fwd_port(eq + 1, rlen - mlen - 1) : first;
	if (first < 0 || last < 0 || to < 0) {
		cg_fwd_bad(err, errlen, tok, len, "not a port (1 to 65535)");
		return 0;
	}
	if (first > last) {
		cg_fwd_bad(err, errlen, tok, len, "the first port is after the last");
		return 0;
	}
	if (to + (last - first) > 65535) {
		cg_fwd_bad(err, errlen, tok, len, "the target range goes past 65535");
		return 0;
	}

	r[0].first = (uint16_t)first;
	r[0].last = (uint16_t)last;
	r[0].to = (uint16_t)to;
	r[0].client = client;
	r[1] = r[0];
	r[1].proto = CG_FWD_UDP; /* used by "both" only */
	return k;
}

/* Parses a forward list as written in a [client] section, e.g.
 * "tcp:9000=22 udp:5000-5010=15000 both:6000 tcp:7000-7010": the rules are
 * appended to out[*n], which has room for max in all, with client set to
 * `client`. Returns 0, or -1 with err (errlen bytes) naming the bad entry
 * and why. On -1 *n is as it was, so a failed list adds nothing. An empty
 * list, or one of separators only, is valid: no rules. Entries are
 * lowercase. A "both" entry that does not fit in what is left of max fails
 * whole. */
static inline int cg_fwd_parse(const char *list, uint8_t client, struct cg_fwd_rule *out, int *n, int max, char *err,
			       size_t errlen)
{
	const char *p = list;
	int n0 = *n;

	if (err && errlen)
		err[0] = '\0';
	while (p && *p) {
		struct cg_fwd_rule r[2];
		size_t len = 0;
		int k;

		while (cg_fwd_sep(*p))
			p++;
		while (p[len] && !cg_fwd_sep(p[len]))
			len++;
		if (!len)
			break;
		k = cg_fwd_entry(p, len, client, r, err, errlen);
		if (!k) {
			*n = n0;
			return -1;
		}
		if (*n + k > max) {
			if (err && errlen)
				snprintf(err, errlen, "too many rules (at most %d)", max);
			*n = n0;
			return -1;
		}
		memcpy(out + *n, r, (size_t)k * sizeof(r[0]));
		*n += k;
		p += len;
	}
	return 0;
}

/* Whether two rules of the same protocol take a common port. The target
 * does not matter: the server's port is what cannot go to two places. */
static inline int cg_fwd_clash(const struct cg_fwd_rule *a, const struct cg_fwd_rule *b)
{
	return a->proto == b->proto && a->first <= b->last && b->first <= a->last;
}

/* The first pair of rules[0..n) that clash, in *i and *j (i < j): returns 1,
 * or 0 when none does and *i and *j are left alone. "First" is by the later
 * rule, then by the earlier one: the rule j is the first that takes a port
 * an earlier rule i already has, the one the script drops, and i is the
 * first it clashes with. O(n^2) is fine: n is at most CG_FWD_MAX and it
 * runs on a load. */
static inline int cg_fwd_overlap(const struct cg_fwd_rule *r, int n, int *i, int *j)
{
	for (int b = 1; b < n; b++) {
		for (int a = 0; a < b; a++) {
			if (cg_fwd_clash(&r[a], &r[b])) {
				*i = a;
				*j = b;
				return 1;
			}
		}
	}
	return 0;
}

/* One client as the IP pass election sees it. */
struct cg_pass_cand {
	int eligible;     /* it may hold IP pass: configured, enabled, passthrough = yes */
	int wish;         /* what its newest session asks for: -1 nothing (yet), 0 off, 1 on */
	uint64_t wish_ms; /* when its wish last turned on (meaningful with wish 1) */
};

/* Who holds the IP pass range: the index of a client in c[0..n), or -1.
 * `current` is the holder now (-1 for none). The holder keeps the range
 * while it is eligible and its wish is not 0 (on, or not known yet, as after
 * the server restarted). Otherwise the eligible client whose wish is 1 and
 * turned on first takes it (equal times: the smallest index), and -1 when
 * none asks. So the range never jumps from a router that keeps asking to one
 * that asked later, and a server that restarted does not hand it to whoever
 * probes first. */
static inline int cg_pass_holder(const struct cg_pass_cand *c, int n, int current)
{
	int best = -1;

	if (current >= 0 && current < n && c[current].eligible && c[current].wish != 0)
		return current;
	for (int i = 0; i < n; i++) {
		if (!c[i].eligible || c[i].wish != 1)
			continue;
		if (best < 0 || c[i].wish_ms < c[best].wish_ms)
			best = i;
	}
	return best;
}

/* The forward table as text into out (outlen bytes, NUL-terminated): the
 * header line, "pass NAME" when holder >= 0, then a "rule" line for each of
 * the n rules whose client is in `writes` (writes[client] nonzero: an
 * enabled client), in the order of rules. names[client] is the name of each
 * client; the holder is written as given, so the caller keeps it among the
 * clients that are written. Returns the length (without the NUL), or -1 when
 * it does not fit, with out empty. */
static inline int cg_fwd_table(char *out, size_t outlen, int holder, const char *const *names,
			       const struct cg_fwd_rule *r, int n, const uint8_t *writes)
{
	size_t len = 0;
	int k;

	if (!outlen)
		return -1;
	k = snprintf(out, outlen, "%s", CG_FWD_HEADER);
	if (k < 0 || (size_t)k >= outlen)
		goto nofit;
	len = (size_t)k;
	if (holder >= 0) {
		k = snprintf(out + len, outlen - len, "pass %s\n", names[holder]);
		if (k < 0 || (size_t)k >= outlen - len)
			goto nofit;
		len += (size_t)k;
	}
	for (int i = 0; i < n; i++) {
		if (!writes[r[i].client])
			continue;
		k = snprintf(out + len, outlen - len, "rule %s %s %u %u %u\n", names[r[i].client],
			     r[i].proto == CG_FWD_TCP ? "tcp" : "udp", (unsigned)r[i].first, (unsigned)r[i].last,
			     (unsigned)r[i].to);
		if (k < 0 || (size_t)k >= outlen - len)
			goto nofit;
		len += (size_t)k;
	}
	return (int)len;
nofit:
	out[0] = '\0';
	return -1;
}

/* The next field of the line [*p, end), fields being separated by spaces
 * and tabs (as the script splits them): sets *f and *flen and moves *p past
 * it. Returns 0 when the line has no more. */
static inline int cg_fwd_field(const char **p, const char *end, const char **f, size_t *flen)
{
	while (*p < end && (**p == ' ' || **p == '\t'))
		(*p)++;
	if (*p >= end)
		return 0;
	*f = *p;
	while (*p < end && **p != ' ' && **p != '\t')
		(*p)++;
	*flen = (size_t)(*p - *f);
	return 1;
}

/* The holder a table names: copies NAME of its first "pass NAME" line into
 * name (namelen bytes) and returns 1. Returns 0, with name empty, when there
 * is none, when the first line is not the header of this format (table 1: a
 * table of another version is not read, as the script does not read it), or
 * when NAME does not fit; text is NUL-terminated. A line that is not
 * exactly "pass" and one name is skipped, as the script skips it, and the
 * first "pass NAME" may follow rules. Used once at start, so that the range
 * stays with its router across a restart of the engine. */
static inline int cg_fwd_table_holder(const char *text, char *name, size_t namelen)
{
	const size_t tlen = sizeof(CG_FWD_TITLE) - 1, vlen = sizeof(CG_FWD_VERSION) - 1;
	const char *p;

	if (!name || !namelen)
		return 0;
	name[0] = '\0';
	/* The version is the whole number: "table 10" is not table 1. */
	if (!text || strncmp(text, CG_FWD_TITLE, tlen) || strncmp(text + tlen, CG_FWD_VERSION, vlen) ||
	    (text[tlen + vlen] >= '0' && text[tlen + vlen] <= '9'))
		return 0;
	/* p rests on the newline that ends the line just read; ++p starts the next. */
	p = strchr(text + tlen + vlen, '\n');
	while (p && *++p) {
		const char *end = strchr(p, '\n'), *f = NULL, *g = NULL;
		size_t flen = 0, glen = 0;

		if (!end)
			end = p + strlen(p);
		if (cg_fwd_field(&p, end, &f, &flen) && flen == 4 && !memcmp(f, "pass", 4) &&
		    cg_fwd_field(&p, end, &g, &glen) && !cg_fwd_field(&p, end, &f, &flen)) {
			if (glen >= namelen)
				return 0;
			memcpy(name, g, glen);
			name[glen] = '\0';
			return 1;
		}
		p = *end ? end : NULL;
	}
	return 0;
}

#endif
