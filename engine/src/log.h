/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_LOG_H
#define CG_LOG_H

#include <stdatomic.h>

enum cg_log_level {
	CG_LOG_ERROR = 0,
	CG_LOG_WARN = 1,
	CG_LOG_INFO = 2,
	CG_LOG_DEBUG = 3,
};

/* Written on start and reload, read by every thread that logs: atomic, with
 * relaxed loads and stores (a level change needs no ordering). Use the two
 * accessors below. */
extern _Atomic int cg_log_level;

static inline int cg_log_level_get(void)
{
	return atomic_load_explicit(&cg_log_level, memory_order_relaxed);
}

static inline void cg_log_level_set(int level)
{
	atomic_store_explicit(&cg_log_level, level, memory_order_relaxed);
}

void cg_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int cg_log_level_parse(const char *s);

#define cg_err(...) cg_log(CG_LOG_ERROR, __VA_ARGS__)
#define cg_warn(...) cg_log(CG_LOG_WARN, __VA_ARGS__)
#define cg_info(...) cg_log(CG_LOG_INFO, __VA_ARGS__)
#define cg_dbg(...)                                                                                \
	do {                                                                                       \
		if (cg_log_level_get() >= CG_LOG_DEBUG)                                            \
			cg_log(CG_LOG_DEBUG, __VA_ARGS__);                                         \
	} while (0)

#endif
