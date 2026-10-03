/* Logs go to stderr; procd and systemd add timestamps and route them.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int cg_log_level = CG_LOG_INFO;

static const char *const names[] = { "error", "warn", "info", "debug" };

void cg_log(int level, const char *fmt, ...)
{
	va_list ap;

	if (level > cg_log_level)
		return;
	fprintf(stderr, "%s: ", names[level]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

int cg_log_level_parse(const char *s)
{
	for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
		if (strcmp(s, names[i]) == 0)
			return i;
	return -1;
}
