/* The one concurrency primitive between data threads: a single-producer,
 * single-consumer ring of fixed-size entries, plus the doorbell that wakes a
 * consumer which said it sleeps, and the space handshake that wakes a
 * producer which stopped because the ring was full.
 *
 * Rules (enforced in review and by tests/test_threads.c):
 * - Indices are free-running uint32_t; prod - cons is the count. The size is
 *   a power of two, at most 2^15.
 * - The producer writes entries at (prod + i) & mask and publishes them once
 *   per batch with a seq_cst store of prod + n. The consumer reads entries,
 *   finishes with any memory they point to, and releases them once per batch
 *   with a seq_cst store of cons + n. Both index stores are seq_cst, and so
 *   are the loads that decide whether to sleep or whether there is room
 *   (each is one side of a Dekker pair); other index loads are acquire.
 * - No atomic_thread_fence anywhere: gcc's TSAN does not model it.
 * - 32-bit atomics only (MIPS32 has no 64-bit ones).
 * - An eventfd only wakes: no ordering depends on it (TSAN treats it as a
 *   synchronisation, so the threaded tests also run a spinning consumer).
 *
 * Doorbell: before an epoll_wait that may block, the consumer stores
 * sleeping = 1, then loads prod of every ring it consumes (seq_cst both); if
 * one has work it stores sleeping = 0 and does not sleep. The producer, after
 * its publish, rings: when it finds sleeping set it exchanges it to 0 and
 * writes the eventfd. In the single total order of seq_cst operations either
 * the consumer sees the new prod or the producer sees sleeping == 1: no lost
 * wake-up.
 *
 * Space handshake (a producer that must not drop, such as a router pump
 * whose receive ring is full: the kernel socket keeps the backlog instead):
 * the producer stores blocked = 1 and re-loads cons (seq_cst both); room
 * after all, it stores blocked = 0 and goes on; still full, it stops reading
 * and sleeps, its pre-sleep check also looking at blocked == 0. The consumer,
 * after its release, exchanges blocked to 0 and, when it was set, writes the
 * producer's eventfd unconditionally: never through the bell's sleeping flag,
 * which the producer may not have set yet (that interleaving would lose the
 * wake-up for good; test_threads forces it).
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_RING_H
#define CG_RING_H

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
	       "cengarde needs lock-free 32-bit and pointer atomics");

#define CG_CACHELINE 64 /* x86-64, Cortex-A72; MIPS 24Kc has 32: 64 is safe */
#define CG_RING_MAX (1u << 15)

struct cg_ring {
	_Alignas(CG_CACHELINE) _Atomic uint32_t prod; /* producer: next index to publish */
	uint32_t cons_seen;                           /* producer's copy of cons */
	_Alignas(CG_CACHELINE) _Atomic uint32_t cons; /* consumer: next index to take */
	uint32_t prod_seen;                           /* consumer's copy of prod */
	_Alignas(CG_CACHELINE) uint32_t mask;         /* size - 1 */
	uint32_t esize;                               /* bytes per entry */
	uint8_t *e;                                   /* entries, allocated with the ring */
};

/* ---- pure parts ---- */

static inline uint32_t cg_ring_count(uint32_t prod, uint32_t cons)
{
	return prod - cons;
}

static inline uint32_t cg_ring_room_of(uint32_t size, uint32_t prod, uint32_t cons)
{
	return size - (prod - cons);
}

static inline int cg_ring_size_ok(uint32_t size)
{
	return size >= 2 && size <= CG_RING_MAX && !(size & (size - 1));
}

/* ---- set-up (before any thread uses it) ---- */

/* size entries of esize bytes (rounded up to a multiple of 8), zeroed and
 * aligned to a cache line. Returns 0, or -1 (bad size, out of memory). */
static inline int cg_ring_init(struct cg_ring *r, uint32_t size, uint32_t esize)
{
	size_t bytes;

	memset(r, 0, sizeof(*r));
	if (!cg_ring_size_ok(size) || !esize)
		return -1;
	esize = (esize + 7u) & ~7u;
	bytes = (size_t)size * esize;
	bytes = (bytes + CG_CACHELINE - 1) / CG_CACHELINE * CG_CACHELINE;
	r->e = aligned_alloc(CG_CACHELINE, bytes);
	if (!r->e)
		return -1;
	memset(r->e, 0, bytes);
	r->mask = size - 1;
	r->esize = esize;
	return 0;
}

static inline void cg_ring_free(struct cg_ring *r)
{
	free(r->e);
	r->e = NULL;
}

static inline uint32_t cg_ring_size(const struct cg_ring *r)
{
	return r->mask + 1;
}

static inline void *cg_ring_at(const struct cg_ring *r, uint32_t idx)
{
	return r->e + (size_t)(idx & r->mask) * r->esize;
}

/* ---- producer ---- */

/* Its own next index (only it stores prod). */
static inline uint32_t cg_ring_prod(struct cg_ring *r)
{
	return atomic_load_explicit(&r->prod, memory_order_relaxed);
}

/* Free entries, reloading cons only when the cached copy shows fewer than
 * want. */
static inline uint32_t cg_ring_room(struct cg_ring *r, uint32_t want)
{
	uint32_t prod = cg_ring_prod(r), room = cg_ring_room_of(r->mask + 1, prod, r->cons_seen);

	if (room < want) {
		r->cons_seen = atomic_load_explicit(&r->cons, memory_order_acquire);
		room = cg_ring_room_of(r->mask + 1, prod, r->cons_seen);
	}
	return room;
}

/* Makes the n entries written after prod visible: once per batch. */
static inline void cg_ring_publish(struct cg_ring *r, uint32_t n)
{
	atomic_store_explicit(&r->prod, cg_ring_prod(r) + n, memory_order_seq_cst);
}

/* ---- consumer ---- */

static inline uint32_t cg_ring_cons(struct cg_ring *r)
{
	return atomic_load_explicit(&r->cons, memory_order_relaxed);
}

/* Entries ready, reloading prod only when the cached copy shows none. */
static inline uint32_t cg_ring_avail(struct cg_ring *r)
{
	uint32_t cons = cg_ring_cons(r), n = cg_ring_count(r->prod_seen, cons);

	if (!n) {
		r->prod_seen = atomic_load_explicit(&r->prod, memory_order_acquire);
		n = cg_ring_count(r->prod_seen, cons);
	}
	return n;
}

/* Gives back the n oldest entries, once the consumer is done with them and
 * with whatever they point to: once per batch. */
static inline void cg_ring_release(struct cg_ring *r, uint32_t n)
{
	atomic_store_explicit(&r->cons, cg_ring_cons(r) + n, memory_order_seq_cst);
}

/* The consumer's pre-sleep check, after it set its bell's sleeping flag. */
static inline int cg_ring_has_work_sc(struct cg_ring *r)
{
	uint32_t prod = atomic_load_explicit(&r->prod, memory_order_seq_cst);

	r->prod_seen = prod;
	return prod != cg_ring_cons(r);
}

/* ---- doorbell ---- */

struct cg_bell {                            /* one per consuming thread */
	_Alignas(CG_CACHELINE) _Atomic uint32_t sleeping;
	int efd;                                /* eventfd (EFD_NONBLOCK) in the consumer's epoll */
};

static inline int cg_bell_init(struct cg_bell *b)
{
	atomic_init(&b->sleeping, 0);
	b->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	return b->efd < 0 ? -1 : 0;
}

static inline void cg_bell_free(struct cg_bell *b)
{
	if (b->efd >= 0)
		close(b->efd);
	b->efd = -1;
}

static inline void cg_efd_write(int efd)
{
	uint64_t one = 1;

	/* EAGAIN: the counter is saturated, so it is readable anyway. */
	while (write(efd, &one, sizeof(one)) < 0 && errno == EINTR)
		;
}

/* Consumer: about to sleep; then its seq_cst checks of every ring. */
static inline void cg_bell_arm(struct cg_bell *b)
{
	atomic_store_explicit(&b->sleeping, 1, memory_order_seq_cst);
}

/* Consumer: awake (or not going to sleep after all). */
static inline void cg_bell_disarm(struct cg_bell *b)
{
	atomic_store_explicit(&b->sleeping, 0, memory_order_seq_cst);
}

/* Consumer: its eventfd was readable. */
static inline void cg_bell_drain(struct cg_bell *b)
{
	uint64_t v;

	if (read(b->efd, &v, sizeof(v)) < 0) {
		/* EAGAIN: someone else drained it */
	}
	cg_bell_disarm(b);
}

/* Producer, after its seq_cst publish: wakes the consumer if it sleeps. The
 * load first keeps the read-modify-write off the path while the consumer is
 * awake (under load it never sleeps). Returns 1 when it wrote the eventfd. */
static inline int cg_bell_ring(struct cg_bell *b)
{
	if (atomic_load_explicit(&b->sleeping, memory_order_seq_cst) &&
	    atomic_exchange_explicit(&b->sleeping, 0, memory_order_seq_cst)) {
		cg_efd_write(b->efd);
		return 1;
	}
	return 0;
}

/* ---- space handshake ---- */

/* Producer, its ring full: returns 0 when there is room after all (go on),
 * 1 when the ring is still full and *blocked is set (stop reading, then
 * sleep; the consumer clears *blocked and writes the eventfd once it made
 * room). */
static inline int cg_ring_block(struct cg_ring *r, _Atomic uint32_t *blocked)
{
	atomic_store_explicit(blocked, 1, memory_order_seq_cst);
	r->cons_seen = atomic_load_explicit(&r->cons, memory_order_seq_cst);
	if (cg_ring_room_of(r->mask + 1, cg_ring_prod(r), r->cons_seen)) {
		atomic_store_explicit(blocked, 0, memory_order_seq_cst);
		return 0;
	}
	return 1;
}

/* Producer: whether the consumer cleared *blocked (seq_cst: part of the
 * pre-sleep check). */
static inline int cg_ring_unblocked(_Atomic uint32_t *blocked)
{
	return !atomic_load_explicit(blocked, memory_order_seq_cst);
}

/* Consumer, after cg_ring_release: when the producer is blocked, clears it
 * and writes its eventfd, unconditionally. At most once per blocked episode.
 * Returns 1 when it did. */
static inline int cg_ring_unblock(_Atomic uint32_t *blocked, int efd)
{
	if (atomic_load_explicit(blocked, memory_order_seq_cst) &&
	    atomic_exchange_explicit(blocked, 0, memory_order_seq_cst)) {
		cg_efd_write(efd);
		return 1;
	}
	return 0;
}

#endif
