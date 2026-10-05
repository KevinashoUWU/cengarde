/* Status snapshot as JSON, written atomically (temp file + rename) so a web
 * UI or LuCI can read it without the daemon opening any port.
 * SPDX-License-Identifier: GPL-2.0-only */
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
void cg_json_ms(struct cg_json *j, const char *key, uint64_t us); /* microseconds as ms with 3 decimals */
void cg_json_ms_signed(struct cg_json *j, const char *key, int64_t us);
void cg_json_bool(struct cg_json *j, const char *key, int v);
void cg_json_null(struct cg_json *j, const char *key);
/* Appends text as it is: plain-text replies use the same buffer. */
void cg_json_raw(struct cg_json *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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
void cg_status_writer_stop(struct cg_status_writer *w);

#endif
