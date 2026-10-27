/* Status snapshot as JSON, written atomically (temp file + rename) so a web
 * UI or LuCI can read it without the daemon opening any port.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_STATUS_H
#define CG_STATUS_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct cg_json {
	char *buf;
	size_t len, cap;
	int failed;
	int depth;
	uint8_t comma[16]; /* per nesting level: an element was already written */
};

void cg_json_init(struct cg_json *j);
void cg_json_free(struct cg_json *j);
/* key is NULL for array elements and the top-level value. */
void cg_json_obj(struct cg_json *j, const char *key);
void cg_json_arr(struct cg_json *j, const char *key);
void cg_json_end(struct cg_json *j, char close);
void cg_json_str(struct cg_json *j, const char *key, const char *val);
void cg_json_u64(struct cg_json *j, const char *key, uint64_t v);
void cg_json_i64(struct cg_json *j, const char *key, int64_t v);
void cg_json_ms(struct cg_json *j, const char *key, uint64_t us); /* microseconds as ms with 3 decimals */
void cg_json_ms_signed(struct cg_json *j, const char *key, int64_t us);
void cg_json_bool(struct cg_json *j, const char *key, int v);
void cg_json_null(struct cg_json *j, const char *key);
/* Appends text as it is: plain-text replies use the same buffer. */
void cg_json_raw(struct cg_json *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* The measurements of story 012, the same at both ends: joint lateness
 * (arrival.h) and WireGuard's datagrams by type and size in each direction
 * (wgsize.h). */
struct cg_arrivals;
struct cg_wgs;
void cg_json_joint(struct cg_json *j, const char *key, const struct cg_arrivals *a);
void cg_json_sizes(struct cg_json *j, const char *key, const struct cg_wgs *up, const struct cg_wgs *down);

/* The receiver's reorderer (reorder.h), the same object at both ends: on or
 * off, what it holds, and its counters and histograms. pool: the client's
 * own pool, whose size and free buffers go in it too; NULL on the server,
 * whose sessions share one pool (shown once, apart). Its 64-bit counters
 * are read without a lock: only from the thread that delivers to
 * WireGuard. */
struct cg_reorder;
struct cg_ro_pool;
void cg_json_reorder(struct cg_json *j, const char *key, const struct cg_reorder *r, int enabled,
		     const struct cg_ro_pool *pool);
/* Which links the reorderer waits for (active.h, clockwork audit C2), the
 * same object at both ends: on (reorder_active), the peer's newest mask,
 * its hold and age (ms, null before one), the links not muted (base) and
 * those rset keeps now, the masks taken, the links evidence opened before
 * a mask said so, the gaps first due that the narrowing let go sooner
 * (early) and those first due at the hold cap (at_cap), and the mask this
 * end sends (sent). now_us: the clock the masks were taken on. */
struct cg_act_rx;
void cg_json_active(struct cg_json *j, const char *key, const struct cg_act_rx *a, const struct cg_reorder *r, int on,
		    uint16_t base, uint16_t rset, uint16_t sent, uint64_t now_us);
/* How far link's receive delay (as the reorderer measures it) is behind the
 * fastest of the links in rset, ms (negative: ahead of them); null until
 * the link and one of rset have a delay. */
void cg_json_owd_spread(struct cg_json *j, const char *key, const struct cg_reorder *r, unsigned link, uint16_t rset);
/* How far above its mean receive delay the reorderer waits for link's
 * packets (CG_RO_DEV_K deviations), ms; null until it has a delay. */
void cg_json_owd_swing(struct cg_json *j, const char *key, const struct cg_reorder *r, unsigned link);
/* A link's capacity in the direction this end sends (capacity.h):
 * capacity_mbit, delivery_mbit (null until measured) and queue_ms (null
 * until a delay). */
struct cg_cap_link;
void cg_json_capacity(struct cg_json *j, const struct cg_cap_link *l, uint32_t target_us, uint64_t now_ms);
/* A link's observations (linksig.h), as "sig": queue_owner, loss_kind,
 * the last verdict's delivery_drop_pct, how long each has stood
 * (since_ms, loss_since_ms; null: never changed) and the counts. */
struct cg_sig_link;
void cg_json_sig(struct cg_json *j, const struct cg_sig_link *s, uint64_t now_ms);
/* The clockwork's last decisions (decision.h), newest first, as
 * "decisions", and their totals by mechanism, as "decision_totals". */
struct cg_decring;
void cg_json_decisions(struct cg_json *j, const struct cg_decring *r, uint64_t now_ms);
/* Guard G4 for one link (split.h), as "g4": its state, its last
 * transition and how long ago, and how many of each. */
struct cg_spread;
void cg_json_g4(struct cg_json *j, const struct cg_spread *s, unsigned link, uint64_t now_ms);
/* The dominant link (dominant.h), as "dominant": its index (null: none),
 * its best delivery over the window and the slower links it dominates
 * (not empty: the split steps aside). */
struct cg_dom;
void cg_json_dominant(struct cg_json *j, const struct cg_dom *d);
/* The freshness of a link's delay reports (health.h): owd_age_ms and
 * owd_reports_per_s. */
struct cg_hlink;
void cg_json_owd_fresh(struct cg_json *j, const struct cg_hlink *h, uint64_t now_ms);

/* Where TCP leaves the tunnel (tcpfree.h's struct cg_tcpe, story 023):
 * handshakes timed, the far leg, SACK holes seen, beyond the tunnel, the
 * tunnel's own and not vouched for, and the ACKs marked CG_F_TUNHOLE. */
struct cg_tcpe;
void cg_json_tcpe(struct cg_json *j, const char *k, const struct cg_tcpe *e);

/* tcp_ack_rule (tcpack.h): whether the peer marks (peer_marks < 0: not
 * said), the ACKs read where the data goes in (with SACK blocks, marked;
 * a NULL: not said), and the retransmissions by row (rows[CG_TCPA_ROWS],
 * copied: of them, copied; NULL: not said). */
struct cg_tcpa;
void cg_json_tcpa(struct cg_json *j, const char *k, const struct cg_tcpa *a, const uint64_t *rows,
		  const uint64_t *copied, int peer_marks);

/* Writes data to path atomically. Returns 0 or -1 (errno set). */
int cg_status_write(const char *path, const char *data, size_t len);

/* Writes snapshots from a thread of its own: a slow disk (an SD card, ext4
 * flushing on rename) must never stall the event loop. */
struct cg_status_writer {
	pthread_t thread;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	char path[256];
	char *buf; /* pending snapshot, owned */
	size_t len;
	int stop, running;
	_Atomic int tid; /* its thread's, for "ctl threads" */
	char name[16];   /* its thread's, as the kernel and "ctl threads" show it */
};

/* Starts the thread, named name (at most 15 characters are kept), with
 * normal scheduling whatever the cpu and rt_priority knobs gave the caller.
 * Returns 0 or -1. */
int cg_status_writer_start(struct cg_status_writer *w, const char *path, const char *name);
/* Hands the text of j over (j is left empty), replacing a snapshot not
 * written yet. Never waits: returns -1, and keeps j, in the rare moment the
 * writer holds the lock to take the previous one. */
int cg_status_writer_submit(struct cg_status_writer *w, struct cg_json *j);
/* Writes what was handed over and not written yet, then ends the thread. */
void cg_status_writer_stop(struct cg_status_writer *w);

#endif
