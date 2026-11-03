/* The 1.0 wire format (story 032, protocol 22): what a datagram between
 * client and server looks like on the network. The engine builds and reads
 * the in-memory packet of proto.h (its 28-byte header, the payload after
 * it) and seals or opens it only here, at the edge:
 *
 *   0       ver(4 bits, CG_PROTO_WIRE) | kind(4 bits, CG_WIRE_KIND_*)
 *   1       client hint    (cg_client_hint(), pair.h: the server picks the
 *                          client before it checks a tag)
 *   2       link id
 *   3..6    session id
 *   7..10   nseq           the low 32 bits of the sender's counter
 *   (kind 1) 11..14        the counter's high 32 bits
 *   (kind 1) 15..22        the sender's epoch (its key's, below)
 *   then, encrypted:
 *           type, flags, seq (4), ts (4), sseq (4)   the inner header, 14
 *           the payload
 *   last    the tag        Poly1305 of ChaCha20-Poly1305 (RFC 8439, aead.h)
 *                          over the clear bytes (its AAD) and the rest
 *
 * The counter: one per sender, session and direction (64 bits, every link,
 * every copy and every packet sent again takes the next one), in the nonce
 * with the direction. The key: BLAKE2s of the shared secret over the
 * direction's label, the session and the sender's epoch, 64 random bits it
 * draws whenever it starts a counter from 0 (the client when it starts, the
 * server for each session it takes and each HELLO): a counter never runs
 * twice under one key, whatever restarts or times out. Kind 1 says the
 * counter whole and the epoch (probes, their replies and HELLO); kind 0,
 * the rest, its low 32 bits, which the receiver widens against the highest
 * it opened under that key (cg_wire_ctr).
 *
 * A receiver checks the tag first (cg_wire_open_head: the tag, then the 14
 * bytes of the inner header decrypted), and decrypts the payload only for
 * a packet its anti-replay takes (cg_wire_open_body): a copy that came first
 * on another link costs a tag, not a body.
 *
 * Pure: no clock, no I/O, no randomness (the caller draws the epochs).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_WIRE_H
#define CG_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "aead.h"
#include "proto.h"

#define CG_WIRE_HDR 11   /* the clear header of kind 0 */
#define CG_WIRE_EXT 12   /* kind 1's: the counter's high 32 bits, the epoch */
#define CG_WIRE_INNER 14 /* the encrypted inner header */
#define CG_WIRE_TAG CG_AEAD_TAG_LEN
#define CG_EPOCH_LEN 8
/* What a packet on the wire weighs more than in memory (kind 0): 13. */
#define CG_WIRE_EXTRA (CG_WIRE_HDR + CG_WIRE_INNER + CG_WIRE_TAG - CG_HDR_LEN)
/* The most a kind 1 adds on top of that. */
#define CG_WIRE_EXTRA_MAX (CG_WIRE_EXTRA + CG_WIRE_EXT)
#define CG_WIRE_HINT_OFF 1
#define CG_WIRE_LINK_OFF 2
#define CG_WIRE_SESSION_OFF 3

enum { CG_WIRE_KIND_PLAIN = 0, CG_WIRE_KIND_EPOCH = 1 };
enum { CG_DIR_C2S = 0, CG_DIR_S2C = 1 };

/* A sender's side of a session and direction. */
struct cg_seal {
	uint8_t key[CG_AEAD_KEY_LEN];
	uint64_t ctr; /* the next packet's */
	uint8_t epoch[CG_EPOCH_LEN];
	uint8_t dir;
};

/* A receiver's: the key of the newest epoch it took and the highest counter
 * opened under it. */
struct cg_unseal {
	uint8_t key[CG_AEAD_KEY_LEN];
	uint64_t top;
	uint8_t epoch[CG_EPOCH_LEN];
	uint8_t dir;
	uint8_t keyed; /* key and epoch hold something */
	uint8_t seen;  /* top holds something */
};

/* The clear part of a packet on the wire (cg_wire_peek). */
struct cg_wire {
	uint8_t kind, hint, link;
	uint32_t session;
	uint32_t nseq;
	uint32_t nhigh;      /* kind 1 */
	uint8_t epoch[CG_EPOCH_LEN]; /* kind 1 (a copy: cg_wire_open_body rewrites the packet) */
	size_t hlen;         /* clear bytes: CG_WIRE_HDR, or with CG_WIRE_EXT */
};

/* The key of direction dir for session, under the sender's epoch, from the
 * shared secret. */
void cg_wire_key(uint8_t out[CG_AEAD_KEY_LEN], const uint8_t secret[CG_KEY_LEN], unsigned dir, uint32_t session,
		 const uint8_t epoch[CG_EPOCH_LEN]);

/* A sender's state from its counter's start (0) under a fresh epoch. */
void cg_seal_init(struct cg_seal *s, const uint8_t secret[CG_KEY_LEN], unsigned dir, uint32_t session,
		  const uint8_t epoch[CG_EPOCH_LEN]);

/* A receiver's state that knows the key of epoch (NULL: none yet; the first
 * kind 1 brings it, cg_unseal_rekey). */
void cg_unseal_init(struct cg_unseal *u, const uint8_t secret[CG_KEY_LEN], unsigned dir, uint32_t session,
		    const uint8_t *epoch);

/* Seals the in-memory packet hdr (CG_HDR_LEN bytes, cg_hdr_put's) and its
 * payload into out (room for plen + CG_HDR_LEN + CG_WIRE_EXTRA_MAX; out
 * must not overlap payload), with the link and the sseq hdr holds now and
 * the next counter of s; kind 1 when epoch1. Returns the wire length. */
size_t cg_wire_seal(uint8_t *out, const uint8_t *hdr, const void *payload, size_t plen, struct cg_seal *s,
		    int epoch1);

/* The clear part of buf (len bytes): 0, or -1 when it is not this version's
 * or too short for a tag and an inner header. */
int cg_wire_peek(struct cg_wire *w, const uint8_t *buf, size_t len);

/* The counter of w: whole from a kind 1, else its nseq widened to the value
 * nearest top (the highest opened; 0 with nothing opened yet). */
uint64_t cg_wire_ctr(const struct cg_wire *w, uint64_t top);

/* Checks the tag of buf (len bytes, peeked into w) under key for counter
 * ctr and direction dir, and decrypts the inner header into h (its link,
 * hint and session from w). 0, or -1 for a forged or damaged packet (buf
 * untouched). */
int cg_wire_open_head(struct cg_hdr *h, const uint8_t *buf, size_t len, const struct cg_wire *w,
		      const uint8_t key[CG_AEAD_KEY_LEN], unsigned dir, uint64_t ctr);

/* After cg_wire_open_head took it: decrypts the payload and turns buf into
 * the in-memory packet (h's header, cg_hdr_put's, and the payload after
 * it). Returns its length, or 0 when the payload does not fit h's type
 * (cg_hdr_plen_ok). */
size_t cg_wire_open_body(uint8_t *buf, size_t len, const struct cg_hdr *h, const struct cg_wire *w,
			 const uint8_t key[CG_AEAD_KEY_LEN], unsigned dir, uint64_t ctr);

/* The key a packet opens under for u: u's own, or, for a kind 1 with
 * another epoch, that epoch's (into tmp). NULL: u has no key and w no
 * epoch. */
const uint8_t *cg_unseal_key(const struct cg_unseal *u, const struct cg_wire *w, const uint8_t secret[CG_KEY_LEN],
			     uint32_t session, uint8_t tmp[CG_AEAD_KEY_LEN]);

/* What a receiver keeps of a packet it took (opened and past its
 * anti-replay): a kind 1 with another epoch makes it u's key, and the
 * counter moves top. */
void cg_unseal_accept(struct cg_unseal *u, const struct cg_wire *w, const uint8_t *key, uint64_t ctr);

#endif
