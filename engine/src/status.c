/* SPDX-License-Identifier: GPL-2.0-only */
#include "status.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h" /* cg_thread_normal */
#include "log.h"
#include "util.h"

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

	cg_thread_normal();
	pthread_mutex_lock(&w->mu);
	for (;;) {
		char *buf;
		size_t len;

		while (!w->buf && !w->stop)
			pthread_cond_wait(&w->cv, &w->mu);
		if (w->stop)
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

int cg_status_writer_start(struct cg_status_writer *w, const char *path)
{
	memset(w, 0, sizeof(*w));
	if (strlen(path) >= sizeof(w->path))
		return -1;
	strcpy(w->path, path);
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
