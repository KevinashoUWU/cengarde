/* Status snapshot as JSON, written atomically (temp file + rename) so a web
 * UI or LuCI can read it without the daemon opening any port.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_STATUS_H
#define CG_STATUS_H

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
void cg_json_bool(struct cg_json *j, const char *key, int v);

/* Writes data to path atomically. Returns 0 or -1 (errno set). */
int cg_status_write(const char *path, const char *data, size_t len);

#endif
