/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_LOG_H
#define CG_LOG_H

enum cg_log_level {
	CG_LOG_ERROR = 0,
	CG_LOG_WARN = 1,
	CG_LOG_INFO = 2,
	CG_LOG_DEBUG = 3,
};

extern int cg_log_level;

void cg_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int cg_log_level_parse(const char *s);

#define cg_err(...) cg_log(CG_LOG_ERROR, __VA_ARGS__)
#define cg_warn(...) cg_log(CG_LOG_WARN, __VA_ARGS__)
#define cg_info(...) cg_log(CG_LOG_INFO, __VA_ARGS__)
#define cg_dbg(...)                                                                                \
	do {                                                                                       \
		if (cg_log_level >= CG_LOG_DEBUG)                                                  \
			cg_log(CG_LOG_DEBUG, __VA_ARGS__);                                         \
	} while (0)

#endif
