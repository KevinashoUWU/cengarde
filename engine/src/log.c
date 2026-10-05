/* Logs go to stderr; procd and systemd add timestamps and route them. Each
 * line is a single write, so lines from the status writer thread and the
 * event loop never interleave.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Rule R3 of the threading design: only 32-bit and pointer atomics, and
 * only where the target has them without locks (MIPS32 and ARMv7 included). */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
	       "int and pointer atomics must be lock-free");

_Atomic int cg_log_level = CG_LOG_INFO;

static const char *const names[] = { "error", "warn", "info", "debug" };

void cg_log(int level, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n, m;

	if (level > cg_log_level_get())
		return;
	n = snprintf(line, sizeof(line), "%s: ", names[level]);
	va_start(ap, fmt);
	m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (m < 0)
		m = 0;
	n += m < (int)(sizeof(line) - (size_t)n - 1) ? m : (int)(sizeof(line) - (size_t)n - 2);
	line[n++] = '\n';
	if (write(STDERR_FILENO, line, (size_t)n) < 0) {
		/* nowhere left to report it */
	}
}

int cg_log_level_parse(const char *s)
{
	for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
		if (strcmp(s, names[i]) == 0)
			return i;
	return -1;
}
