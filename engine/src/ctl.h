/* Control socket: a Unix stream socket that takes one command per
 * connection, as one line of text, and answers with text ("error: ..." when
 * the command failed) before closing the connection.
 *
 *   status                 the status JSON, as written to status_file
 *   links                  client: every interface and why it carries the
 *                          tunnel or not; server: the sessions and their links
 *   link NAME off|on|auto  client: pause an uplink, use it although the
 *                          configuration leaves it out, or back to what the
 *                          configuration says. Kept across reloads, lost on a
 *                          restart.
 *   reset                  client: every uplink back to the configuration
 *   reload                 re-read the configuration, as SIGHUP does
 *   threads                every thread of the engine: its TID, the CPU it
 *                          last ran on, its CPU time and its share of a CPU
 *                          over the last 5 s
 *
 * The command parser and the table of manual overrides are pure functions
 * with unit tests (tests/test_ctl.c); the socket itself is in ctl.c.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_CTL_H
#define CG_CTL_H

#include <ctype.h>
#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CG_CTL_LINE 256 /* longest command line */
#define CG_CTL_OVERRIDES 32
#define CG_CTL_DEFAULT_SOCKET "/var/run/cengarde/cengarde.sock"

enum cg_ctl_op { CG_CTL_STATUS = 1, CG_CTL_LINKS, CG_CTL_LINK, CG_CTL_RESET, CG_CTL_RELOAD, CG_CTL_THREADS };

/* Manual state of an uplink, over what the configuration says. */
enum cg_ovr { CG_OVR_AUTO = 0, CG_OVR_OFF, CG_OVR_ON };

struct cg_ctl_cmd {
	enum cg_ctl_op op;
	char ifname[IFNAMSIZ]; /* link */
	enum cg_ovr ovr;       /* link */
};

/* A name the kernel accepts for an interface (its dev_valid_name). */
static inline int cg_ifname_valid(const char *s)
{
	size_t n = strlen(s);

	if (!n || n >= IFNAMSIZ || !strcmp(s, ".") || !strcmp(s, ".."))
		return 0;
	for (; *s; s++)
		if (*s == '/' || *s == ':' || isspace((unsigned char)*s))
			return 0;
	return 1;
}

/* Parses one command line (a trailing newline is fine). Returns 0, or -1
 * with a message in err. */
static inline int cg_ctl_parse(const char *line, struct cg_ctl_cmd *cmd, char *err, size_t errlen)
{
	static const struct {
		const char *name;
		enum cg_ctl_op op;
	} plain[] = { { "status", CG_CTL_STATUS },
		      { "links", CG_CTL_LINKS },
		      { "reset", CG_CTL_RESET },
		      { "reload", CG_CTL_RELOAD },
		      { "threads", CG_CTL_THREADS } };
	char buf[CG_CTL_LINE], *w[4], *p = buf;
	int n = 0;

	memset(cmd, 0, sizeof(*cmd));
	if (strlen(line) >= sizeof(buf)) {
		snprintf(err, errlen, "command too long");
		return -1;
	}
	strcpy(buf, line);
	for (;;) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		if (n == (int)(sizeof(w) / sizeof(w[0]))) {
			snprintf(err, errlen, "too many words");
			return -1;
		}
		w[n++] = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if (*p)
			*p++ = '\0';
	}
	if (!n) {
		snprintf(err, errlen, "empty command");
		return -1;
	}
	for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
		if (strcmp(w[0], plain[i].name))
			continue;
		if (n != 1) {
			snprintf(err, errlen, "'%s' takes no arguments", w[0]);
			return -1;
		}
		cmd->op = plain[i].op;
		return 0;
	}
	if (strcmp(w[0], "link")) {
		snprintf(err, errlen, "unknown command '%.32s' (status, links, link, reset, reload, threads)", w[0]);
		return -1;
	}
	if (n != 3) {
		snprintf(err, errlen, "usage: link NAME off|on|auto");
		return -1;
	}
	if (!cg_ifname_valid(w[1])) {
		snprintf(err, errlen, "'%.32s' is not an interface name", w[1]);
		return -1;
	}
	if (!strcmp(w[2], "off"))
		cmd->ovr = CG_OVR_OFF;
	else if (!strcmp(w[2], "on"))
		cmd->ovr = CG_OVR_ON;
	else if (!strcmp(w[2], "auto"))
		cmd->ovr = CG_OVR_AUTO;
	else {
		snprintf(err, errlen, "link %s: expected off, on or auto", w[1]);
		return -1;
	}
	cmd->op = CG_CTL_LINK;
	strcpy(cmd->ifname, w[1]);
	return 0;
}

/* Manual overrides by interface name, also for interfaces not there yet. */
struct cg_ovr_table {
	int n;
	struct {
		char name[IFNAMSIZ];
		enum cg_ovr v;
	} e[CG_CTL_OVERRIDES];
};

static inline enum cg_ovr cg_ovr_get(const struct cg_ovr_table *t, const char *name)
{
	for (int i = 0; i < t->n; i++)
		if (!strcmp(t->e[i].name, name))
			return t->e[i].v;
	return CG_OVR_AUTO;
}

/* Sets the override of name; CG_OVR_AUTO removes it. Returns 0, or -1 when
 * the table is full. */
static inline int cg_ovr_set(struct cg_ovr_table *t, const char *name, enum cg_ovr v)
{
	int n = t->n < CG_CTL_OVERRIDES ? t->n : CG_CTL_OVERRIDES, i = 0;

	while (i < n && strcmp(t->e[i].name, name))
		i++;
	if (v == CG_OVR_AUTO) {
		if (i < n)
			t->e[i] = t->e[--n];
		t->n = n;
		return 0;
	}
	if (i == n) {
		size_t len = strlen(name);

		if (n == CG_CTL_OVERRIDES || len >= sizeof(t->e[0].name))
			return -1;
		memcpy(t->e[i].name, name, len + 1);
		t->n = n + 1;
	}
	t->e[i].v = v;
	return 0;
}

static inline void cg_ovr_reset(struct cg_ovr_table *t)
{
	t->n = 0;
}

/* Whether an interface carries the tunnel: its override, when it has one,
 * else what the configuration says (configured). */
static inline int cg_ovr_wanted(enum cg_ovr o, int configured)
{
	return o == CG_OVR_ON || (o == CG_OVR_AUTO && configured);
}

static inline const char *cg_ovr_name(enum cg_ovr o)
{
	return o == CG_OVR_OFF ? "off" : o == CG_OVR_ON ? "on" : "auto";
}

/* ---- the socket (ctl.c) ---- */

#define CG_CTL_CONNS 4                  /* connections at a time; more are closed at once */
#define CG_CTL_TIMEOUT_MS 5000          /* to send the command and take the reply */
#define CG_CTL_RELOAD_TIMEOUT_MS 60000  /* reload: names in the configuration may wait on DNS */

struct cg_ctl_conn {
	int fd;      /* -1: free */
	int waiting; /* the command was taken; its reply comes later (reload) */
	uint64_t deadline_ms;
	size_t inlen;
	char in[CG_CTL_LINE];
	char *out;
	size_t outlen, outoff;
};

struct cg_ctl {
	int fd; /* listening socket, -1: none */
	char path[108];
	struct cg_ctl_conn c[CG_CTL_CONNS];
};

void cg_ctl_init(struct cg_ctl *ctl);

/* Listens on path, owner only, in epoll set ep: the socket as
 * CG_EV(CG_EV_CTL, CG_CTL_CONNS), connection i as CG_EV(CG_EV_CTL, i).
 * Replaces a stale socket, never a live one or another kind of file, and
 * creates a missing last directory. Returns 0 or -1 with err set. */
int cg_ctl_open(struct cg_ctl *ctl, const char *path, int ep, char *err, size_t errlen);

/* Closes every connection and the socket, and removes the socket file. */
void cg_ctl_close(struct cg_ctl *ctl, int ep);

/* Handles readiness of index idx. Returns the connection whose command line
 * has arrived (NUL-terminated in c[i].in), which then needs cg_ctl_reply, or
 * -1. */
int cg_ctl_event(struct cg_ctl *ctl, int ep, uint32_t idx, uint64_t now_ms);

/* Replies on connection i with len bytes of text (malloc'ed, owned from
 * here on; NULL just closes) and closes the connection once they are sent. */
void cg_ctl_reply(struct cg_ctl *ctl, int ep, int i, char *text, size_t len);

/* Replies msg to every connection waiting for a deferred reply. */
void cg_ctl_reply_waiting(struct cg_ctl *ctl, int ep, const char *msg);

/* Closes connections past their deadline. */
void cg_ctl_expire(struct cg_ctl *ctl, int ep, uint64_t now_ms);

/* The command line side: sends line and waits up to timeout_ms for the whole
 * reply. Returns 0 with the reply in *reply (malloc'ed, *len bytes plus a
 * NUL), or -1 with err set. */
int cg_ctl_request(const char *path, const char *line, int timeout_ms, char **reply, size_t *len, char *err,
		   size_t errlen);

#endif
