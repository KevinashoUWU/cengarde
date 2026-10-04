/* INI-style configuration, e.g.:
 *
 *   mode = client
 *   key = <base64 of 32 random bytes, see "cengarde genkey">
 *   server = 203.0.113.10:59402 [2001:db8::4]:59402
 *   interfaces = eth1.* wwan*
 *
 *   [link eth1.10]
 *   label = WOM
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_CONFIG_H
#define CG_CONFIG_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "proto.h"
#include "srvpick.h" /* CG_MAX_CANDS */

#define CG_MAX_PATTERNS 32
#define CG_MAX_LINK_CFG 32
/* Entries of a server list, addresses or names; a name takes at most
 * CG_NAME_ADDRS addresses of each family, and a list CG_MAX_CANDS in all. */
#define CG_MAX_SERVERS 8
#define CG_NAME_ADDRS 4

/* ---- generic INI layer ---- */

struct cg_ini_entry {
	char *section; /* "" for the global section */
	char *key;
	char *value;
	int line;
	int used;
};

struct cg_ini {
	struct cg_ini_entry *e;
	int n, cap;
};

int cg_ini_parse(struct cg_ini *ini, const char *text, char *err, size_t errlen);
/* Last value of key in section (sections are matched exactly), or NULL. */
const char *cg_ini_get(struct cg_ini *ini, const char *section, const char *key);
void cg_ini_free(struct cg_ini *ini);

/* ---- typed configuration ---- */

enum cg_mode { CG_MODE_CLIENT = 1, CG_MODE_SERVER = 2 };

struct cg_link_cfg {
	char name[IFNAMSIZ];
	char label[64];
	int enabled;
	int nserver; /* 0: the global list */
	struct sockaddr_storage server[CG_MAX_CANDS];
	int nentry;                      /* entries of server as written */
	uint8_t entry_n[CG_MAX_SERVERS]; /* how many addresses each gave, in order */
};

struct cg_config {
	int mode;
	uint8_t key[CG_KEY_LEN];
	char description[128];
	struct sockaddr_storage listen;
	char status_file[256];
	uint32_t status_interval_ms;
	char control_socket[108]; /* "": none (ctl.h) */
	int rcvbuf;
	int log_level;

	/* link health, each end for its send direction (health.h) */
	uint32_t mute_behind_ms; /* 0: never mute for delay */
	uint32_t unmute_behind_ms;
	uint32_t mute_settle_ms;
	uint32_t mute_trickle;
	uint32_t min_active_links;

	/* latency knobs */
	uint32_t busy_poll_us; /* poll without sleeping this long after traffic */
	int cpu;               /* pin to this CPU; -1: any */
	uint32_t rt_priority;  /* SCHED_FIFO priority; 0: normal scheduling */

	/* client */
	int nserver; /* addresses, in the order of preference (srvpick.h) */
	struct sockaddr_storage server[CG_MAX_CANDS];
	int nentry;                      /* entries of server as written */
	uint8_t entry_n[CG_MAX_SERVERS]; /* how many addresses each gave, in order */
	uint32_t server_failover_ms; /* without a reply, a link tries its next server address; 0: never */
	char *include[CG_MAX_PATTERNS];
	int ninclude;
	char *exclude[CG_MAX_PATTERNS];
	int nexclude;
	struct cg_link_cfg links[CG_MAX_LINK_CFG];
	int nlinks;
	uint32_t probe_interval_ms; /* while there is traffic */
	uint32_t probe_idle_ms;     /* while there is none */
	int sndbuf;
	int passthrough; /* IP pass asked of the server: -1 nothing, 0 off, 1 on */

	/* server */
	struct sockaddr_storage wireguard;
	uint32_t max_sessions;
	uint32_t session_timeout_ms;
	uint32_t path_timeout_ms;
	char passthrough_file[256]; /* where the IP pass the client asks for goes; "": nowhere */

	char *strings; /* storage behind include/exclude */
};

/* Parses text (file contents). Unknown keys are reported in warn (one per
 * line, may be truncated) but are not fatal. Returns 0 or -1 with err set. */
int cg_config_parse(struct cg_config *c, const char *text, char *err, size_t errlen, char *warn,
		    size_t warnlen);
int cg_config_load(struct cg_config *c, const char *path, char *err, size_t errlen, char *warn,
		   size_t warnlen);
void cg_config_free(struct cg_config *c);

/* Settings for an interface: its [link NAME] section, or NULL. */
const struct cg_link_cfg *cg_config_link(const struct cg_config *c, const char *ifname);

/* A reload applies b over a running a, except what the event loop set up
 * once: returns the first such setting that differs (the process restarts
 * for it), or NULL. */
const char *cg_config_restart_needed(const struct cg_config *a, const struct cg_config *b);

/* The value of a global key in the file at path, without checking the rest
 * of it (and without resolving any name in it). Returns 0, 1 when the key
 * is not there, or -1 with err set. */
int cg_config_peek(const char *path, const char *key, char *out, size_t outlen, char *err, size_t errlen);

#endif
