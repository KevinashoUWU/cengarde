/* SPDX-License-Identifier: GPL-2.0-only */
#include "status.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void cg_json_init(struct cg_json *j)
{
	memset(j, 0, sizeof(*j));
}

void cg_json_free(struct cg_json *j)
{
	free(j->buf);
	memset(j, 0, sizeof(*j));
}

static void put(struct cg_json *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct cg_json *j, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (j->failed)
		return;
	for (;;) {
		size_t room = j->cap - j->len;

		va_start(ap, fmt);
		n = vsnprintf(j->buf ? j->buf + j->len : NULL, room, fmt, ap);
		va_end(ap);
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

void cg_json_bool(struct cg_json *j, const char *k, int v)
{
	key(j, k);
	put(j, "%s", v ? "true" : "false");
}

int cg_status_write(const char *path, const char *data, size_t len)
{
	char tmp[512];
	int fd;
	ssize_t w;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	w = write(fd, data, len);
	if (close(fd) < 0 || w != (ssize_t)len || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}
