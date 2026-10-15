/* INI-style configuration, e.g.:
 *
 *   mode = client
 *   key = <base64 of 32 random bytes, see "cengarde genkey">
 *   server = vps.example.com:59402 203.0.113.10:59402 [2001:db8::4]:59402
 *   interfaces = eth1.* wwan*
 *
 *   [link eth1.10]
 *   label = WOM
 *   resolver = 192.168.8.1
 *
 * A name in "server" is looked up by the client itself, through the links
 * (resolve.h, names.c), again as its TTL says and at once when the server
 * stops answering on its addresses; /etc/hosts goes first, once, when the
 * file is read.
 *
 * A server serves one client with the global key, or several, one
 * [client NAME] section each:
 *
 *   mode = server
 *   forward_file = /var/lib/cengarde/forward
 *   wireguard_port_file = /var/lib/cengarde/wg/ports
 *
 *   [client alpha]
 *   key = <the router's key>
 *   wireguard = 127.0.0.1:65501
 *   wireguard_poke = 10.79.12.34:9
 *   forward = tcp:9000=22 udp:5000-5010
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_CONFIG_H
#define CG_CONFIG_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "clients.h"  /* CG_MAX_CLIENTS */
#include "dns.h"      /* CG_DNS_NAME_MAX */
#include "fwdtable.h" /* struct cg_fwd_rule */
#include "proto.h"
#include "srvpick.h" /* CG_MAX_CANDS */

#define CG_MAX_PATTERNS 32
#define CG_MAX_LINK_CFG 32
#define CG_MAX_NAMES 8     /* names in all the server lists */
#define CG_MAX_RESOLVERS 8 /* DNS servers in a resolver list */

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

/* How the client handles its link sockets (pump.h, thrplan.h): legacy is
 * the loop of 0.4 (the default until the per-link threads are measured on
 * the Pi); off, the per-link structure in one thread; on, a thread per
 * link; auto, what thrplan.h picks for this machine. */
enum cg_link_threads { CG_LT_AUTO = 0, CG_LT_ON, CG_LT_OFF, CG_LT_LEGACY };

static inline const char *cg_lt_name(int v)
{
	return v == CG_LT_ON ? "on" : v == CG_LT_OFF ? "off" : v == CG_LT_LEGACY ? "legacy" : "auto";
}

/* How the send direction is shared out among the links (split.h, story
 * 012). A client takes off, on or force; a server, off, client or force.
 * on is the field mode of a client: it splits its upload once the server
 * says it reorders, and asks the server to split the download. client is the
 * server's: it splits the download when the client asks for it and reorders.
 * force splits whatever the other end says, evenly, for the lab. off
 * duplicates everything, as before. The ends tell each other in the
 * probes' bonding tail (proto.h); split.h decides (cg_split_active). */
enum cg_split_mode { CG_SPLIT_OFF = 0, CG_SPLIT_ON, CG_SPLIT_CLIENT, CG_SPLIT_FORCE };

static inline const char *cg_split_name(int v)
{
	return v == CG_SPLIT_ON ? "on" : v == CG_SPLIT_CLIENT ? "client" : v == CG_SPLIT_FORCE ? "force" : "off";
}

struct cg_link_cfg {
	char name[IFNAMSIZ];
	char label[64];
	int enabled;
	/* Its share of a split stream with split = on, 0 to 1000 (split.h), up
	 * and, sent in the probes, down; force splits evenly. 0 makes it carry
	 * a copy of every packet. An interface with no [link] section has 1. */
	uint32_t weight;
	int nsrv; /* entries of its "server" as written; 0: the global list */
	struct cg_srvent srv[CG_MAX_SERVERS];
	/* The list built from srv and the names' addresses (cg_config_build). */
	int nserver;
	struct sockaddr_storage server[CG_MAX_CANDS];
	int nentry;                      /* = nsrv */
	uint8_t entry_n[CG_MAX_SERVERS]; /* how many addresses each gave, in order */
	int nresolver;                   /* its own DNS servers (the carrier's), asked before the global ones */
	struct sockaddr_storage resolver[CG_MAX_RESOLVERS];
	int cpu;                         /* pins its thread (link_threads = on); -1: none */
};

/* A name of the server lists, lowercase without a trailing dot. One that
 * /etc/hosts lists takes its addresses from there and is never looked up. */
struct cg_name_cfg {
	char name[CG_DNS_NAME_MAX + 1];
	int hosts;
	struct cg_nameaddrs addrs; /* hosts only */
};

/* A client of a server, a [client NAME] section. Its WireGuard address is
 * the global one unless it has its own. */
#define CG_CLIENT_NAME 32     /* room for a name, with its NUL */
#define CG_CLIENT_NAME_MAX 12 /* the longest name: cg-NAME fits in IFNAMSIZ */
struct cg_client_cfg {
	char name[CG_CLIENT_NAME];
	char label[64];
	uint8_t key[CG_KEY_LEN];
	int enabled;     /* no: its probes are refused (a HELLO with CG_F_REFUSED) */
	int passthrough; /* may hold IP pass */
	struct sockaddr_storage wireguard;
	struct sockaddr_storage wireguard_poke; /* AF_UNSPEC: none */
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

	/* Reordering what arrives and splitting what is sent (story 012), both
	 * ends. Each end reorders what it receives with them (reorder.h): the
	 * client its download, the server each session's upload; and split
	 * shares out what each end sends (split.h). */
	int reorder;                  /* put the peer's DATA back in sequence before WireGuard */
	uint32_t reorder_max_wait_ms; /* the longest a missing packet holds back the ones behind it */
	uint32_t reorder_dup_wait_ms; /* the same where nothing is split (<= reorder_max_wait_ms) */
	uint32_t reorder_margin_ms;   /* added to the slowest link's delay when waiting for a gap */
	uint32_t reorder_bypass;      /* data datagrams of up to this many bytes skip the reorderer */
	int reorder_by_link;          /* a gap waits for the link the next packet says it went on (reorder.h) */
	int reorder_adaptive;         /* each link's margin learns from its late packets (reorder.h) */
	uint32_t rescue_kb;           /* server: the ring of DATA sent lately, for NACKs (rescue.h); 0: none */
	uint32_t rescue_pct;          /* server: what a rescue may cost, % of the download */
	uint32_t rescue_wait_ms;      /* a gap about to go is asked for and waits this long; 0: never */
	uint32_t rescue_tries;        /* asked for this many times within rescue_wait_ms */
	uint32_t reorder_pool_kb;     /* memory for the packets it holds; changing it restarts */
	int split;                    /* enum cg_split_mode */
	uint32_t split_small;         /* datagrams of up to this many bytes are never split */
	uint32_t split_spread_ms;     /* only links this close to the fastest share the split (split.h G4); 0: off */
	int split_spread_copy;        /* links out of the split get every packet (split.h); default: not with governor */
	int governor;                 /* the split's weights from each link's queue (govern.h) */
	int capacity_weights;         /* the split's weights from each link's capacity (capacity.h) */
	int split_eta;                /* split by expected arrival (eta.h) */
	int pacing;                   /* each link's socket paced at its rate (pace.h) */
	uint32_t governor_target_ms;  /* the queue a link may hold */
	uint32_t governor_window_ms;  /* of each link's minimum delay */

	/* latency knobs */
	uint32_t busy_poll_us; /* poll without sleeping this long after traffic */
	int cpu;               /* pin to this CPU; -1: any */
	uint32_t rt_priority;  /* SCHED_FIFO priority; 0: normal scheduling */

	/* client */
	int nsrv; /* entries of "server" as written, in the order of preference */
	struct cg_srvent srv[CG_MAX_SERVERS];
	/* The list built from srv and the names' addresses (srvpick.h,
	 * cg_config_build). */
	int nserver;
	struct sockaddr_storage server[CG_MAX_CANDS];
	int nentry;                      /* = nsrv */
	uint8_t entry_n[CG_MAX_SERVERS]; /* how many addresses each gave, in order */
	int nnames; /* the names of every server list, in order of appearance */
	struct cg_name_cfg names[CG_MAX_NAMES];
	int nresolver; /* DNS servers every link may ask, after its own */
	struct sockaddr_storage resolver[CG_MAX_RESOLVERS];
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
	int link_threads; /* enum cg_link_threads */
	uint32_t io_queue; /* entries of each hub -> pump ring, a power of two */

	/* server */
	struct sockaddr_storage wireguard; /* AF_UNSPEC: none (every [client] has its own) */
	uint32_t max_sessions; /* a limit, applied in place by a reload */
	uint32_t session_timeout_ms;
	uint32_t path_timeout_ms;
	char passthrough_file[256]; /* where the IP pass the client asks for goes; "": nowhere */
	/* [client NAME] sections, in their order (nclients 0: one client, the
	 * global key), and their forward rules, by client. */
	struct cg_client_cfg *clients;
	int nclients;
	struct cg_fwd_rule *forward;
	int nforward;
	char forward_file[256]; /* the forward table (fwdtable.h); "": none */
	char wireguard_port_file[256]; /* each client's port WireGuard knows (wgport.h); "": none */
	uint32_t lanes;             /* listen sockets steered by link id (steer.h); 1: one socket */
	/* The client's address in the tunnel: a datagram there makes WireGuard
	 * start a handshake when it ignores the client's (wgwatch.h). Family
	 * AF_UNSPEC: never. */
	struct sockaddr_storage wireguard_poke;

	char *strings; /* storage behind include/exclude */
};

/* Where /etc/hosts is (tests point it elsewhere). */
extern const char *cg_hosts_path;

/* Builds the server lists of c (global and of each [link]) from their
 * entries and names[i], the addresses of c->names[i] now (NULL: none). */
void cg_config_build(struct cg_config *c, const struct cg_nameaddrs *const *names);

/* The [client NAME] section called name, or NULL. */
const struct cg_client_cfg *cg_config_client(const struct cg_config *c, const char *name);

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
