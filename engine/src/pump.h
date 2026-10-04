/* Router link pumps: the I/O of the link sockets, and nothing else.
 *
 * A pump owns some link sockets: it reads them (recvmmsg) into its receive
 * ring of slots, stamping each batch with the time it was read, and closes a
 * socket when told to. It decides nothing: the hub, the one thread that
 * holds the session (sequence, MAC, anti-replay window, health, probes),
 * takes the slots in turn (clientpath.h) and sends the first copies to
 * WireGuard straight out of them.
 *
 * - link_threads = on: one pump thread per link, at most CG_MAX_PUMPS (a
 *   later link shares the pump with the fewest links, thrplan.h). A stall of
 *   one link, or of its thread, fills only that link's socket: the others
 *   go on. A stall of the hub still holds every link at once (each link's
 *   kernel buffer absorbs it): that is inherent to one window owner.
 * - link_threads = off: the same code with one virtual pump that owns every
 *   link, run by the hub itself in its own epoll, without a thread or a
 *   doorbell.
 *
 * Hub -> pump: one ring (cmdq) of 32-bit entries. Bit 31 set is a command,
 * bits 0-2 its slot in cmd[], written before the publish; packets come with
 * PR 3c. At most CG_PUMP_CMDS commands are in flight: past that the pump is
 * stalled and the hub keeps one pending state per link instead of a queue.
 * Commands: OPEN(link, gen, fd), CLOSE(link, gen), MODE (reserved: only
 * "drop" exists until bonding) and STOP. The pump, never the hub, closes a
 * socket it was handed: the hub stops using its number before it sends
 * CLOSE, so the two never race a close against a send. A socket the pump
 * cannot add to its epoll is kept, unpolled, until that CLOSE comes, for the
 * same reason, and the link's bit goes up in open_failed.
 *
 * Pump -> hub: the receive ring; each slot carries (link, gen), and the hub
 * drops a slot whose generation is not its link's current one (the hub moves
 * the generation on every open and on every close). With its ring full a
 * pump thread stops reading its sockets (the kernel keeps the backlog and
 * drops only that link's copies) through the space handshake of ring.h.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_PUMP_H
#define CG_PUMP_H

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "engine.h"
#include "ring.h"

#define CG_MAX_PUMPS 8
#define CG_PUMP_CMDS 8
#define CG_PUMP_RXQ 128       /* receive slots of a pump thread (270 KB) */
#define CG_PUMP_RXQ_INLINE 64 /* of the one virtual pump of link_threads = off */
#define CG_PUMP_ENT_CMD 0x80000000u
#define CG_PUMP_STACK (128 * 1024)

/* A datagram a pump read, as the hub takes it. 64-byte aligned. */
struct cg_rxslot {
	uint64_t t_us;  /* when its batch was read */
	uint16_t len;
	uint8_t link;   /* link id */
	uint8_t gen;    /* generation of the link's socket it came from */
	uint8_t trunc;  /* the kernel cut it (MSG_TRUNC) */
	uint8_t pad[64 - 13];
	uint8_t buf[CG_BUF];
};
_Static_assert(sizeof(struct cg_rxslot) % 64 == 0, "receive slots are cache-line multiples");

enum cg_pump_op { CG_PUMP_OPEN = 1, CG_PUMP_CLOSE, CG_PUMP_MODE, CG_PUMP_STOP };

struct cg_pump_cmd {
	uint8_t op;   /* enum cg_pump_op */
	uint8_t link; /* link id */
	uint8_t gen;  /* OPEN: generation its slots carry */
	uint8_t mode; /* MODE: reserved */
	int fd;       /* OPEN */
};

struct cg_pump {
	/* Shared through ring.h: each index on its own cache line. */
	struct cg_ring cmdq; /* hub -> pump */
	struct cg_ring rxq;  /* pump -> hub: struct cg_rxslot */
	struct cg_bell bell; /* the pump's own, rung by the hub (thread only) */
	_Alignas(CG_CACHELINE) _Atomic uint32_t rx_blocked; /* space handshake (ring.h) */
	_Atomic uint32_t open_failed; /* links whose socket its epoll refused; the hub takes them */
	_Atomic uint32_t stop;
	/* Its liveness and counters: one writer (the pump), relaxed stores,
	 * 32-bit; the hub keeps the totals (cg_acc32). */
	_Alignas(CG_CACHELINE) _Atomic uint32_t loop_ms; /* clock of its last loop pass (ms, wraps) */
	_Atomic uint32_t rx_pkts, rx_paused, rx_errors;
	_Atomic int rx_errno; /* last unexpected receive error */
	_Atomic int tid;

	/* Written by the hub before it publishes the entry that names it. */
	struct cg_pump_cmd cmd[CG_PUMP_CMDS];

	/* The hub's own. */
	_Alignas(CG_CACHELINE) uint32_t cmd_head, cmd_tail; /* commands posted, and the oldest maybe in flight */
	uint32_t cmd_at[CG_PUMP_CMDS];                     /* their entries' indices in cmdq */

	/* The pump's own (its thread, or the hub inline). */
	_Alignas(CG_CACHELINE) int ep; /* its epoll; inline: the hub's */
	int threaded;
	struct cg_bell *hub;      /* rung after each publish (thread only) */
	int fd[CG_MAX_LINKS];     /* the socket it holds per link id, -1: none */
	uint8_t gen[CG_MAX_LINKS];
	uint16_t held;            /* links it holds a socket for */
	uint16_t failed;          /* of those, the ones its epoll refused (kept, not polled) */
	int paused;               /* receive ring full: its sockets are out of the poll */
	uint32_t n_pkts, n_paused, n_errors; /* the counters' running values */
	uint32_t busy_poll_us;
	int cpu;              /* pin to this CPU; -1: the CPUs below */
	uint32_t rt_priority; /* SCHED_FIFO priority; 0: normal */
	cpu_set_t cpus;       /* the process's CPUs before the hub was pinned */
	int have_cpus;
	char name[16];
	struct mmsghdr msg[CG_BATCH];
	struct iovec iov[CG_BATCH];
	pthread_t thread;
	int started;

	/* Tests only (tests/test_threads.c): called at the points where the
	 * space handshake can be raced, and switches that take out one of its
	 * two safeguards to show the test would catch it. */
	void (*hook)(struct cg_pump *p, int where);
	int test_flags;
};

enum { CG_PUMP_HOOK_BLOCKED = 1, CG_PUMP_HOOK_SLEEP };
#define CG_PUMP_TEST_NO_PRESLEEP 1 /* the pre-sleep check ignores rx_blocked */

/* Sets p up before any thread uses it. threaded: a thread of its own
 * (cg_pump_start), with cmdq entries in its command ring; else run inline
 * by the hub in the hub's epoll ep. rxq: receive slots. Returns 0 or -1. */
int cg_pump_init(struct cg_pump *p, int threaded, int ep, uint32_t rxq, uint32_t cmdq);
/* Closes what it holds and frees it: inline, or after cg_pump_stop. */
void cg_pump_free(struct cg_pump *p);

/* Starts the thread of a threaded pump, named name (at most 15 characters
 * are kept), which rings hub after each publish. p->cpu, p->rt_priority,
 * p->busy_poll_us and p->cpus are set before. Returns 0 or -1. */
int cg_pump_start(struct cg_pump *p, const char *name, struct cg_bell *hub);
/* Stops the thread: 0, or -1 when it did not stop within timeout_ms (the
 * caller must then leave everything it may still use alone). */
int cg_pump_stop(struct cg_pump *p, int timeout_ms);
/* CPU time of its thread, ns; 0 when unknown. */
uint64_t cg_pump_cpu_ns(struct cg_pump *p);

/* ---- the pump's side (its thread, or the hub inline) ---- */

void cg_pump_cmd(struct cg_pump *p, const struct cg_pump_cmd *c);
/* Reads the socket of link into the receive ring, up to rounds batches, and
 * publishes each batch. Returns the datagrams read. */
int cg_pump_rx(struct cg_pump *p, unsigned link, int rounds);

/* ---- the hub's side ---- */

/* Hands a command to the pump (inline: runs it at once). Returns 0, or -1
 * when CG_PUMP_CMDS commands are still in flight: the pump is stalled, and
 * the hub keeps the command pending. */
int cg_pump_post(struct cg_pump *p, const struct cg_pump_cmd *c);
/* Commands posted and not taken yet. */
uint32_t cg_pump_cmds_waiting(struct cg_pump *p);

static inline uint32_t cg_pump_rx_avail(struct cg_pump *p)
{
	return cg_ring_avail(&p->rxq);
}

/* The i-th slot ready, 0 the oldest. */
static inline struct cg_rxslot *cg_pump_rx_slot(struct cg_pump *p, uint32_t i)
{
	return cg_ring_at(&p->rxq, cg_ring_cons(&p->rxq) + i);
}

/* Gives back the n oldest slots, once the hub is done with them (after its
 * sendmmsg to WireGuard returned: the slots are the iovecs). */
static inline void cg_pump_rx_release(struct cg_pump *p, uint32_t n)
{
	cg_ring_release(&p->rxq, n);
	if (p->threaded)
		cg_ring_unblock(&p->rx_blocked, p->bell.efd);
}

#endif
