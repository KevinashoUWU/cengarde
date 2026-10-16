/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "status.h"

#include "capacity.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arrival.h"
#include "engine.h" /* cg_thread_normal */
#include "log.h"
#include "util.h"
#include "wgsize.h"

void cg_json_init(struct cg_json *j)
{
	memset(j, 0, sizeof(*j));
}

void cg_json_free(struct cg_json *j)
{
	free(j->buf);
	memset(j, 0, sizeof(*j));
}

static void vput(struct cg_json *j, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));

static void vput(struct cg_json *j, const char *fmt, va_list ap)
{
	va_list aq;
	int n;

	if (j->failed)
		return;
	for (;;) {
		size_t room = j->cap - j->len;

		va_copy(aq, ap);
		n = vsnprintf(j->buf ? j->buf + j->len : NULL, room, fmt, aq);
		va_end(aq);
		if (n < 0) {
			j->failed = 1;
			return;
		}
		if ((size_t)n < room) {
			j->len += (size_t)n;
			return;
		}
		size_t cap = j->cap ? j->cap * 2 : 4096;
		while (cap - j->len <= (size_t)n)
			cap *= 2;
		char *nb = realloc(j->buf, cap);
		if (!nb) {
			j->failed = 1;
			return;
		}
		j->buf = nb;
		j->cap = cap;
	}
}

static void put(struct cg_json *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct cg_json *j, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vput(j, fmt, ap);
	va_end(ap);
}

void cg_json_raw(struct cg_json *j, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vput(j, fmt, ap);
	va_end(ap);
}

static void key(struct cg_json *j, const char *k)
{
	if (j->comma[j->depth])
		put(j, ",");
	j->comma[j->depth] = 1;
	if (k)
		put(j, "\"%s\":", k); /* keys are literals from our code */
}

void cg_json_obj(struct cg_json *j, const char *k)
{
	key(j, k);
	put(j, "{");
	if (j->depth + 1 < (int)sizeof(j->comma))
		j->comma[++j->depth] = 0;
}

void cg_json_arr(struct cg_json *j, const char *k)
{
	key(j, k);
	put(j, "[");
	if (j->depth + 1 < (int)sizeof(j->comma))
		j->comma[++j->depth] = 0;
}

void cg_json_end(struct cg_json *j, char close)
{
	put(j, "%c", close);
	if (j->depth > 0)
		j->depth--;
}

void cg_json_str(struct cg_json *j, const char *k, const char *v)
{
	key(j, k);
	put(j, "\"");
	for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
		if (*p == '"' || *p == '\\')
			put(j, "\\%c", *p);
		else if (*p < 0x20)
			put(j, "\\u%04x", *p);
		else
			put(j, "%c", *p);
	}
	put(j, "\"");
}

void cg_json_u64(struct cg_json *j, const char *k, uint64_t v)
{
	key(j, k);
	put(j, "%" PRIu64, v);
}

void cg_json_ms(struct cg_json *j, const char *k, uint64_t us)
{
	key(j, k);
	put(j, "%" PRIu64 ".%03u", us / 1000, (unsigned)(us % 1000));
}

void cg_json_ms_signed(struct cg_json *j, const char *k, int64_t us)
{
	uint64_t a = us < 0 ? (uint64_t)-(us + 1) + 1 : (uint64_t)us;

	key(j, k);
	put(j, "%s%" PRIu64 ".%03u", us < 0 ? "-" : "", a / 1000, (unsigned)(a % 1000));
}

void cg_json_bool(struct cg_json *j, const char *k, int v)
{
	key(j, k);
	put(j, "%s", v ? "true" : "false");
}

void cg_json_null(struct cg_json *j, const char *k)
{
	key(j, k);
	put(j, "null");
}

/* The nonzero buckets of a histogram by link mask, keyed by the mask in
 * decimal. */
static void json_masks(struct cg_json *j, const char *k, const uint64_t h[CG_JOINT_MASKS])
{
	char m[12];

	cg_json_obj(j, k);
	for (unsigned i = 0; i < CG_JOINT_MASKS; i++) {
		if (!h[i])
			continue;
		snprintf(m, sizeof(m), "%u", i);
		cg_json_u64(j, m, h[i]);
	}
	cg_json_end(j, '}');
}

void cg_json_joint(struct cg_json *j, const char *k, const struct cg_arrivals *a)
{
	cg_json_obj(j, k);
	cg_json_arr(j, "deadlines_ms");
	for (unsigned d = 0; d < CG_JOINT_D; d++)
		cg_json_u64(j, NULL, cg_joint_deadline_us(d) / 1000);
	cg_json_end(j, ']');
	cg_json_u64(j, "packets", a->evaluated);
	json_masks(j, "expect", a->expect);
	cg_json_arr(j, "late");
	for (unsigned d = 0; d < CG_JOINT_D; d++)
		json_masks(j, NULL, a->late[d]);
	cg_json_end(j, ']');
	cg_json_u64(j, "other", a->other);
	cg_json_end(j, '}');
}

static void json_wgs(struct cg_json *j, const char *k, const struct cg_wgs *w)
{
	cg_json_obj(j, k);
	cg_json_arr(j, "data");
	for (unsigned i = 0; i < CG_WGS_N; i++)
		cg_json_u64(j, NULL, w->data[i]);
	cg_json_end(j, ']');
	cg_json_arr(j, "data_bytes");
	for (unsigned i = 0; i < CG_WGS_N; i++)
		cg_json_u64(j, NULL, w->data_bytes[i]);
	cg_json_end(j, ']');
	cg_json_u64(j, "init", w->msg[CG_WGS_INIT]);
	cg_json_u64(j, "response", w->msg[CG_WGS_RESPONSE]);
	cg_json_u64(j, "cookie", w->msg[CG_WGS_COOKIE]);
	cg_json_u64(j, "other", w->msg[CG_WGS_OTHER]);
	cg_json_end(j, '}');
}

void cg_json_sizes(struct cg_json *j, const char *k, const struct cg_wgs *up, const struct cg_wgs *down)
{
	cg_json_obj(j, k);
	cg_json_arr(j, "edges");
	for (unsigned i = 0; i < CG_WGS_N - 1; i++)
		cg_json_u64(j, NULL, cg_wgs_edge(i));
	cg_json_end(j, ']');
	json_wgs(j, "up", up);
	json_wgs(j, "down", down);
	cg_json_end(j, '}');
}

static void json_hist(struct cg_json *j, const char *k, const uint64_t h[CG_RO_HIST])
{
	cg_json_arr(j, k);
	for (unsigned i = 0; i < CG_RO_HIST; i++)
		cg_json_u64(j, NULL, h[i]);
	cg_json_end(j, ']');
}

void cg_json_reorder(struct cg_json *j, const char *k, const struct cg_reorder *r, int enabled,
		     const struct cg_ro_pool *pool)
{
	const struct cg_ro_stats *st = &r->st;

	cg_json_obj(j, k);
	cg_json_bool(j, "enabled", enabled);
	cg_json_u64(j, "held", r->held);
	cg_json_u64(j, "held_max", st->held_max);
	if (pool) {
		cg_json_u64(j, "pool_buffers", pool->n);
		cg_json_u64(j, "pool_free", pool->nfree);
	}
	cg_json_u64(j, "in_order", st->in_order);
	cg_json_u64(j, "held_total", st->held_total);
	cg_json_u64(j, "bypassed", st->bypassed);
	cg_json_u64(j, "split_rx", st->split_rx);
	cg_json_obj(j, "lost");
	cg_json_u64(j, "rack", st->lost_rack);
	cg_json_u64(j, "cap", st->lost_cap);
	cg_json_u64(j, "overflow", st->lost_overflow);
	cg_json_u64(j, "flush", st->lost_flush);
	cg_json_end(j, '}');
	/* adaptive: each link's learned margin, by link id */
	cg_json_arr(j, "adapt_ms");
	for (unsigned i = 0; i < CG_MAX_LINKS; i++)
		cg_json_ms(j, NULL, r->link[i].adapt_us);
	cg_json_end(j, ']');
	cg_json_obj(j, "late");
	cg_json_u64(j, "after_rack", st->late_rack);
	cg_json_u64(j, "after_cap", st->late_cap);
	cg_json_u64(j, "after_overflow", st->late_overflow);
	cg_json_u64(j, "old", st->late_old);
	cg_json_end(j, '}');
	cg_json_u64(j, "far", st->far);
	cg_json_u64(j, "resyncs", st->resyncs);
	cg_json_u64(j, "pool_full", st->pool_full);
	/* The rescue (rescue.h): gaps asked for again, that came, that did not. */
	cg_json_obj(j, "rescue");
	cg_json_u64(j, "asked", st->asked);
	cg_json_u64(j, "rescued", st->rescued);
	cg_json_u64(j, "lost", st->ask_lost);
	cg_json_u64(j, "refused", st->ask_refused);
	cg_json_end(j, '}');
	cg_json_u64(j, "max_burst", st->max_burst);
	cg_json_arr(j, "edges_ms");
	for (unsigned i = 0; i < CG_RO_HIST - 1; i++)
		cg_json_u64(j, NULL, cg_ro_edge_ms(i));
	cg_json_end(j, ']');
	json_hist(j, "wait", st->wait);
	json_hist(j, "late_by", st->late_by);
	cg_json_end(j, '}');
}

void cg_json_owd_spread(struct cg_json *j, const char *k, const struct cg_reorder *r, unsigned link, uint16_t rset)
{
	const struct cg_ro_link *l = link < CG_MAX_LINKS ? &r->link[link] : NULL;
	uint32_t fastest = 0;
	int any = 0;

	for (unsigned i = 0; rset; i++, rset >>= 1)
		if ((rset & 1) && r->link[i].owd_ok && (!any || (int32_t)(r->link[i].owd - fastest) < 0)) {
			fastest = r->link[i].owd;
			any = 1;
		}
	if (!l || !l->owd_ok || !any)
		cg_json_null(j, k);
	else
		cg_json_ms_signed(j, k, (int32_t)(l->owd - fastest));
}

void cg_json_owd_swing(struct cg_json *j, const char *k, const struct cg_reorder *r, unsigned link)
{
	const struct cg_ro_link *l = link < CG_MAX_LINKS ? &r->link[link] : NULL;

	if (!l || !l->owd_ok)
		cg_json_null(j, k);
	else
		cg_json_ms(j, k, cg_ro_swing(l));
}

void cg_json_capacity(struct cg_json *j, const struct cg_cap_link *l, uint32_t target_us)
{
	/* bps / 1000 as "ms" prints Mbit/s with three decimals. */
	if (l->have_rate) {
		cg_json_ms(j, "capacity_mbit", cg_cap_bps(l) / 1000);
		cg_json_ms(j, "delivery_mbit", l->rate_bps / 1000);
	} else {
		cg_json_null(j, "capacity_mbit");
		cg_json_null(j, "delivery_mbit");
	}
	if (l->queue_us >= 0)
		cg_json_ms(j, "queue_ms", (uint64_t)l->queue_us);
	else
		cg_json_null(j, "queue_ms");
	/* the queue's slope (ms per second) and the rescue's weight, 0 to 1 */
	if (l->have_s1)
		cg_json_ms_signed(j, "queue_slope_ms_s", l->s1);
	else
		cg_json_null(j, "queue_slope_ms_s");
	/* where split_predict takes the queue to be in a second */
	if (cg_cap_pred_us(l) >= 0)
		cg_json_ms(j, "queue_pred_ms", (uint64_t)cg_cap_pred_us(l));
	else
		cg_json_null(j, "queue_pred_ms");
	cg_json_ms(j, "rescue_weight", (uint64_t)cg_cap_rescue_w(l, target_us) * 1000 / CG_CAP_W_ONE);
}

int cg_status_write(const char *path, const char *data, size_t len)
{
	char tmp[512];
	int fd;
	ssize_t w;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0 && errno == ENOENT) {
		/* The last directory, such as /var/run/cengarde, may not be there yet. */
		char dir[512], *slash;

		strcpy(dir, tmp);
		slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			if (mkdir(dir, 0755) == 0)
				fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		}
	}
	if (fd < 0)
		return -1;
	w = write(fd, data, len);
	if (close(fd) < 0 || w != (ssize_t)len || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void *writer_main(void *arg)
{
	struct cg_status_writer *w = arg;
	struct cg_ratelimit rl = { 0 };

	w->tid = cg_gettid();
	pthread_setname_np(pthread_self(), w->name);
	cg_thread_normal();
	pthread_mutex_lock(&w->mu);
	for (;;) {
		char *buf;
		size_t len;

		while (!w->buf && !w->stop)
			pthread_cond_wait(&w->cv, &w->mu);
		/* What was handed on before the stop is written first: the last
		 * port WireGuard knows a client by has to outlive the engine. */
		if (!w->buf)
			break;
		buf = w->buf;
		len = w->len;
		w->buf = NULL;
		pthread_mutex_unlock(&w->mu);
		if (cg_status_write(w->path, buf, len) < 0 && cg_ratelimit_ok(&rl, cg_now_ms(), 60000))
			cg_warn("status file %s: %s", w->path, strerror(errno));
		free(buf);
		pthread_mutex_lock(&w->mu);
	}
	pthread_mutex_unlock(&w->mu);
	return NULL;
}

int cg_status_writer_start(struct cg_status_writer *w, const char *path, const char *name)
{
	memset(w, 0, sizeof(*w));
	if (strlen(path) >= sizeof(w->path))
		return -1;
	strcpy(w->path, path);
	snprintf(w->name, sizeof(w->name), "%s", name);
	if (pthread_mutex_init(&w->mu, NULL))
		return -1;
	if (pthread_cond_init(&w->cv, NULL)) {
		pthread_mutex_destroy(&w->mu);
		return -1;
	}
	if (pthread_create(&w->thread, NULL, writer_main, w)) {
		pthread_cond_destroy(&w->cv);
		pthread_mutex_destroy(&w->mu);
		return -1;
	}
	w->running = 1;
	return 0;
}

int cg_status_writer_submit(struct cg_status_writer *w, struct cg_json *j)
{
	char *old;

	if (!w->running || j->failed || !j->buf || pthread_mutex_trylock(&w->mu))
		return -1;
	old = w->buf;
	w->buf = j->buf;
	w->len = j->len;
	pthread_cond_signal(&w->cv);
	pthread_mutex_unlock(&w->mu);
	j->buf = NULL;
	j->len = j->cap = 0;
	free(old);
	return 0;
}

void cg_status_writer_stop(struct cg_status_writer *w)
{
	if (!w->running)
		return;
	pthread_mutex_lock(&w->mu);
	w->stop = 1;
	pthread_cond_signal(&w->cv);
	pthread_mutex_unlock(&w->mu);
	pthread_join(w->thread, NULL);
	free(w->buf);
	w->buf = NULL;
	pthread_cond_destroy(&w->cv);
	pthread_mutex_destroy(&w->mu);
	w->running = 0;
}
