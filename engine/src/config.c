/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "log.h"
#include "resolve.h" /* cg_hosts_find */
#include "steer.h"   /* lanes */
#include "util.h"

const char *cg_hosts_path = "/etc/hosts";

/* Every link may ask these after its own (Cloudflare and Quad9). */
static const char default_resolvers[] = "1.1.1.1 9.9.9.9 [2606:4700:4700::1111] [2620:fe::fe]";

/* Interfaces that never carry tunnel traffic. */
static const char default_exclude[] = "lo wg* docker* veth* br-* virbr* tun* tap* ifb* dummy*";

/* ---- INI ---- */

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static int valid_key(const char *k)
{
	if (!*k)
		return 0;
	for (; *k; k++)
		if (!isalnum((unsigned char)*k) && *k != '_' && *k != '-' && *k != '.')
			return 0;
	return 1;
}

static int ini_add(struct cg_ini *ini, const char *section, const char *key, const char *value, int line)
{
	struct cg_ini_entry *e;

	if (ini->n == ini->cap) {
		int cap = ini->cap ? ini->cap * 2 : 32;
		struct cg_ini_entry *ne = realloc(ini->e, sizeof(*ne) * (size_t)cap);

		if (!ne)
			return -1;
		ini->e = ne;
		ini->cap = cap;
	}
	e = &ini->e[ini->n];
	e->section = strdup(section);
	e->key = strdup(key);
	e->value = strdup(value);
	e->line = line;
	e->used = 0;
	if (!e->section || !e->key || !e->value) {
		free(e->section);
		free(e->key);
		free(e->value);
		return -1;
	}
	ini->n++;
	return 0;
}

int cg_ini_parse(struct cg_ini *ini, const char *text, char *err, size_t errlen)
{
	char *copy = strdup(text), *save = NULL, *line, section[96] = "";
	int lineno = 0, rc = 0;

	memset(ini, 0, sizeof(*ini));
	if (!copy) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	/* strtok_r would skip empty lines and lose the line numbers. */
	for (line = copy; line; line = save) {
		char *nl = strchr(line, '\n'), *s, *eq, *key, *val;

		save = nl ? nl + 1 : NULL;
		if (nl)
			*nl = '\0';
		lineno++;
		s = trim(line);
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			char *close = strrchr(s, ']'), *src, *dst;
			size_t n = 0;

			if (!close || close[1]) {
				snprintf(err, errlen, "line %d: malformed section header", lineno);
				rc = -1;
				break;
			}
			*close = '\0';
			/* Collapse inner whitespace: "[link   eth1]" == "[link eth1]". */
			for (src = trim(s + 1), dst = section; *src && n + 1 < sizeof(section); src++) {
				if (isspace((unsigned char)*src)) {
					if (n && dst[-1] == ' ')
						continue;
					*dst++ = ' ';
				} else {
					*dst++ = *src;
				}
				n++;
			}
			*dst = '\0';
			if (!*section || *src) {
				snprintf(err, errlen, "line %d: bad section name", lineno);
				rc = -1;
				break;
			}
			continue;
		}
		eq = strchr(s, '=');
		if (!eq) {
			snprintf(err, errlen, "line %d: expected 'key = value'", lineno);
			rc = -1;
			break;
		}
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);
		if (!valid_key(key)) {
			snprintf(err, errlen, "line %d: invalid key '%s'", lineno, key);
			rc = -1;
			break;
		}
		if (*val == '"') {
			char *q = strchr(val + 1, '"');

			if (!q || *trim(q + 1)) {
				snprintf(err, errlen, "line %d: unterminated quoted value", lineno);
				rc = -1;
				break;
			}
			*q = '\0';
			val++;
		} else {
			for (char *p = val; *p; p++)
				if (*p == '#' && p > val && isspace((unsigned char)p[-1])) {
					*p = '\0';
					val = trim(val);
					break;
				}
		}
		if (ini_add(ini, section, key, val, lineno) < 0) {
			snprintf(err, errlen, "out of memory");
			rc = -1;
			break;
		}
	}
	free(copy);
	if (rc)
		cg_ini_free(ini);
	return rc;
}

const char *cg_ini_get(struct cg_ini *ini, const char *section, const char *key)
{
	const char *v = NULL;

	for (int i = 0; i < ini->n; i++)
		if (!strcmp(ini->e[i].section, section) && !strcmp(ini->e[i].key, key)) {
			ini->e[i].used = 1;
			v = ini->e[i].value;
		}
	return v;
}

void cg_ini_free(struct cg_ini *ini)
{
	for (int i = 0; i < ini->n; i++) {
		free(ini->e[i].section);
		free(ini->e[i].key);
		free(ini->e[i].value);
	}
	free(ini->e);
	memset(ini, 0, sizeof(*ini));
}

/* ---- typed configuration ---- */

static int get_u32(struct cg_ini *ini, const char *sec, const char *key, uint32_t min, uint32_t max,
		   uint32_t *out, char *err, size_t errlen)
{
	const char *v = cg_ini_get(ini, sec, key);
	char *end;
	unsigned long long n;

	if (!v)
		return 0;
	errno = 0;
	n = strtoull(v, &end, 10);
	if (!*v || *end || errno || n < min || n > max) {
		snprintf(err, errlen, "%s: expected an integer between %u and %u, got '%s'", key, min, max, v);
		return -1;
	}
	*out = (uint32_t)n;
	return 0;
}

static int get_bool(struct cg_ini *ini, const char *sec, const char *key, int *out, char *err, size_t errlen)
{
	const char *v = cg_ini_get(ini, sec, key);

	if (!v)
		return 0;
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1"))
		*out = 1;
	else if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "0"))
		*out = 0;
	else {
		snprintf(err, errlen, "%s: expected yes or no, got '%s'", key, v);
		return -1;
	}
	return 0;
}

static int get_str(struct cg_ini *ini, const char *sec, const char *key, char *out, size_t outlen, char *err,
		   size_t errlen)
{
	const char *v = cg_ini_get(ini, sec, key);

	if (!v)
		return 0;
	if (strlen(v) >= outlen) {
		snprintf(err, errlen, "%s: value too long (max %zu)", key, outlen - 1);
		return -1;
	}
	strcpy(out, v);
	return 0;
}

/* Parses a list of at most max entries into out, which holds cap addresses.
 * A name may stand for several addresses, at most CG_NAME_ADDRS of each
 * family, and takes the slots the later entries leave, so every entry is
 * parsed, checked and kept. Peers are addresses to send to: names are allowed
 * there (getaddrinfo, the server's keys), the wildcard and multicast
 * addresses are not. The client's server lists are get_servers'. */
static int get_addrs(struct cg_ini *ini, const char *sec, const char *key, int peers,
		     struct sockaddr_storage *out, int max, int cap, int *n, char *err, size_t errlen)
{
	const char *v = cg_ini_get(ini, sec, key);
	char buf[1024], *items[CG_MAX_SERVERS * 2], e[256];
	struct sockaddr_storage got[CG_MAX_CANDS];
	int count;

	*n = 0;
	if (!v)
		return 0;
	if (strlen(v) >= sizeof(buf)) {
		snprintf(err, errlen, "%s: value too long", key);
		return -1;
	}
	strcpy(buf, v);
	count = cg_split_list(buf, items, (int)CG_ARRAY_SIZE(items));
	if (count > max) { /* an error, never dropped in silence */
		if (max == 1)
			snprintf(err, errlen, "%s: expected a single address", key);
		else
			snprintf(err, errlen, "%s: at most %d addresses", key, max);
		return -1;
	}
	for (int i = 0; i < count; i++) {
		/* count <= max <= cap: a slot is left for each later entry. */
		int room = cap - *n - (count - 1 - i), per[2] = { 0, 0 };
		int k = cg_addr_parse(items[i], peers, got, (int)CG_ARRAY_SIZE(got), e, sizeof(e));
		const char *why = NULL;

		if (k < 0) {
			snprintf(err, errlen, "%s: %s", key, e);
			return -1;
		}
		for (int j = 0; peers && j < k && !why; j++)
			why = cg_addr_unfit_peer(&got[j]);
		if (why) {
			snprintf(err, errlen, "%s: '%s' is %s", key, items[i], why);
			return -1;
		}
		for (int j = 0; j < k && room > 0; j++) {
			int *f = &per[got[j].ss_family == AF_INET6];

			if (*f == CG_NAME_ADDRS)
				continue;
			(*f)++;
			room--;
			out[(*n)++] = got[j];
		}
	}
	if (!*n) {
		snprintf(err, errlen, "%s: no address given", key);
		return -1;
	}
	return 0;
}

static char *read_file(const char *path, char *err, size_t errlen);

/* The hosts file, read at most once per configuration; NULL when there is
 * none (no name is in it, then). */
struct hosts {
	int read;
	char *text;
};

static const char *hosts_text(struct hosts *h)
{
	char e[256];

	if (!h->read) {
		h->read = 1;
		h->text = read_file(cg_hosts_path, e, sizeof(e));
	}
	return h->text;
}

/* The index of name among c's names, added (with what the hosts file gives
 * it) when new; -1 with err set when there are too many. */
static int name_index(struct cg_config *c, struct hosts *h, const char *key, const char *name, char *err,
		      size_t errlen)
{
	struct cg_name_cfg *nc;

	for (int i = 0; i < c->nnames; i++)
		if (!strcmp(c->names[i].name, name))
			return i;
	if (c->nnames == CG_MAX_NAMES) {
		snprintf(err, errlen, "%s: at most %d names in all the server lists", key, CG_MAX_NAMES);
		return -1;
	}
	nc = &c->names[c->nnames];
	memset(nc, 0, sizeof(*nc));
	strcpy(nc->name, name);
	nc->hosts = cg_hosts_find(hosts_text(h), name, &nc->addrs);
	return c->nnames++;
}

/* A server list as written: at most max entries, each an address (no
 * wildcard, no multicast) or a name, kept for the client to look up
 * (resolve.h). Sets *n to how many; none when the key is not there. */
static int get_servers(struct cg_ini *ini, const char *sec, struct cg_config *c, struct hosts *h,
		       struct cg_srvent *out, int max, int *n, char *err, size_t errlen)
{
	const char *v = cg_ini_get(ini, sec, "server"), *why;
	char buf[1024], *items[CG_MAX_SERVERS * 2], host[256], e[256];
	int count;

	*n = 0;
	if (!v)
		return 0;
	if (strlen(v) >= sizeof(buf)) {
		snprintf(err, errlen, "server: value too long");
		return -1;
	}
	strcpy(buf, v);
	count = cg_split_list(buf, items, (int)CG_ARRAY_SIZE(items));
	if (count > max) { /* an error, never dropped in silence */
		snprintf(err, errlen, "server: at most %d addresses", max);
		return -1;
	}
	if (!count) {
		snprintf(err, errlen, "server: no address given");
		return -1;
	}
	for (int i = 0; i < count; i++) {
		struct cg_srvent *s = &out[i];
		char name[CG_DNS_NAME_MAX + 1];

		memset(s, 0, sizeof(*s));
		s->name = -1;
		if (cg_hostport(items[i], host, sizeof(host), &s->port, e, sizeof(e)) < 0) {
			snprintf(err, errlen, "server: %s", e);
			return -1;
		}
		if (!strcmp(host, "*")) {
			snprintf(err, errlen, "server: '%s' is the wildcard address", items[i]);
			return -1;
		}
		if (cg_addr_literal(host, s->port, &s->a) == 0) {
			why = cg_addr_unfit_peer(&s->a);
			if (why) {
				snprintf(err, errlen, "server: '%s' is %s", items[i], why);
				return -1;
			}
		} else if (items[i][0] == '[' || cg_dns_name_norm(host, name, sizeof(name)) < 0) {
			snprintf(err, errlen, "server: '%s' is neither an address nor a name", items[i]);
			return -1;
		} else if ((s->name = name_index(c, h, "server", name, err, errlen)) < 0) {
			return -1;
		}
	}
	*n = count;
	return 0;
}

/* DNS servers: addresses with an optional port (53), "[...]" or bare for
 * IPv6. Sets *n; dflt when the key is not there, none when it is empty. */
static int get_resolvers(struct cg_ini *ini, const char *sec, const char *dflt, struct sockaddr_storage *out,
			 int *n, char *err, size_t errlen)
{
	const char *v = cg_ini_get(ini, sec, "resolver"), *why;
	char buf[1024], *items[CG_MAX_RESOLVERS * 2], e[256];
	int count;

	*n = 0;
	if (!v)
		v = dflt;
	if (!v)
		return 0;
	if (strlen(v) >= sizeof(buf)) {
		snprintf(err, errlen, "resolver: value too long");
		return -1;
	}
	strcpy(buf, v);
	count = cg_split_list(buf, items, (int)CG_ARRAY_SIZE(items));
	if (count > CG_MAX_RESOLVERS) {
		snprintf(err, errlen, "resolver: at most %d addresses", CG_MAX_RESOLVERS);
		return -1;
	}
	for (int i = 0; i < count; i++) {
		size_t len = strlen(items[i]);

		/* "[2001:db8::53]": bare, but in brackets. */
		if (len > 2 && items[i][0] == '[' && items[i][len - 1] == ']') {
			memmove(items[i], items[i] + 1, len - 2);
			items[i][len - 2] = '\0';
		}
		if (cg_addr_literal(items[i], 53, &out[i]) < 0 &&
		    cg_addr_parse(items[i], 0, &out[i], 1, e, sizeof(e)) < 0) {
			snprintf(err, errlen, "resolver: '%s' is not an address", items[i]);
			return -1;
		}
		why = cg_addr_unfit_peer(&out[i]);
		if (why) {
			snprintf(err, errlen, "resolver: '%s' is %s", items[i], why);
			return -1;
		}
	}
	*n = count;
	return 0;
}

void cg_config_build(struct cg_config *c, const struct cg_nameaddrs *const *names)
{
	const struct cg_nameaddrs *own[CG_MAX_NAMES];

	/* Without addresses given, those of the hosts file (a name that is
	 * not there gives none). */
	if (!names) {
		for (int i = 0; i < c->nnames; i++)
			own[i] = c->names[i].hosts ? &c->names[i].addrs : NULL;
		names = own;
	}
	c->nserver = cg_srv_build(c->srv, c->nsrv, names, c->server, c->entry_n);
	c->nentry = c->nsrv;
	for (int i = 0; i < c->nlinks; i++) {
		struct cg_link_cfg *l = &c->links[i];

		l->nserver = cg_srv_build(l->srv, l->nsrv, names, l->server, l->entry_n);
		l->nentry = l->nsrv;
	}
}

const struct cg_link_cfg *cg_config_link(const struct cg_config *c, const char *ifname)
{
	for (int i = 0; i < c->nlinks; i++)
		if (!strcmp(c->links[i].name, ifname))
			return &c->links[i];
	return NULL;
}

static int parse_links(struct cg_config *c, struct cg_ini *ini, struct hosts *h, char *err, size_t errlen)
{
	for (int i = 0; i < ini->n; i++) {
		const char *sec = ini->e[i].section, *name;
		struct cg_link_cfg *l;

		if (strncmp(sec, "link ", 5))
			continue;
		name = sec + 5;
		if (cg_config_link(c, name))
			continue;
		if (strlen(name) >= IFNAMSIZ) {
			snprintf(err, errlen, "[%s]: interface name too long", sec);
			return -1;
		}
		if (c->nlinks == CG_MAX_LINK_CFG) {
			snprintf(err, errlen, "too many [link] sections (max %d)", CG_MAX_LINK_CFG);
			return -1;
		}
		l = &c->links[c->nlinks++];
		memset(l, 0, sizeof(*l));
		strcpy(l->name, name);
		l->enabled = 1;
		l->weight = 1;
		l->cpu = -1;
		if (cg_ini_get(ini, sec, "cpu")) {
			uint32_t cpu;

			if (get_u32(ini, sec, "cpu", 0, 1023, &cpu, err, errlen))
				return -1;
			l->cpu = (int)cpu;
		}
		if (get_str(ini, sec, "label", l->label, sizeof(l->label), err, errlen) ||
		    get_bool(ini, sec, "enabled", &l->enabled, err, errlen) ||
		    get_u32(ini, sec, "weight", 0, 1000, &l->weight, err, errlen) ||
		    get_servers(ini, sec, c, h, l->srv, CG_MAX_SERVERS, &l->nsrv, err, errlen) ||
		    get_resolvers(ini, sec, NULL, l->resolver, &l->nresolver, err, errlen))
			return -1;
	}
	return 0;
}

const struct cg_client_cfg *cg_config_client(const struct cg_config *c, const char *name)
{
	for (int i = 0; i < c->nclients; i++)
		if (!strcmp(c->clients[i].name, name))
			return &c->clients[i];
	return NULL;
}

/* Whether ini has a [client NAME] section (or a bare [client]). */
static int has_clients(const struct cg_ini *ini)
{
	for (int i = 0; i < ini->n; i++)
		if (!strncmp(ini->e[i].section, "client", 6) && (!ini->e[i].section[6] || ini->e[i].section[6] == ' '))
			return 1;
	return 0;
}

/* A client's name goes into the forward table, where cengarde-nat takes
 * only the names cengarde-vps-setup gives (is_name: its cg-NAME has to fit
 * in an interface name): 1 to CG_CLIENT_NAME_MAX lowercase letters, digits
 * and '-', not starting with '-', and not "default". Anything else would
 * load here and then forward nothing, in silence. */
static int valid_client_name(const char *n)
{
	size_t len = strlen(n);

	if (!len || len > CG_CLIENT_NAME_MAX || n[0] == '-' || !strcmp(n, "default"))
		return 0;
	for (; *n; n++)
		if (!(*n >= 'a' && *n <= 'z') && !(*n >= '0' && *n <= '9') && *n != '-')
			return 0;
	return 1;
}

static const char *proto_name(uint8_t p)
{
	return p == CG_FWD_TCP ? "tcp" : "udp";
}

/* The [client NAME] sections of a server, in order, with their forward
 * rules; called with c->wireguard already parsed (AF_UNSPEC: none). */
static int parse_clients(struct cg_config *c, struct cg_ini *ini, char *err, size_t errlen)
{
	int fi, fj;

	c->clients = calloc(CG_MAX_CLIENTS, sizeof(*c->clients));
	c->forward = calloc(CG_FWD_MAX, sizeof(*c->forward));
	if (!c->clients || !c->forward) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	for (int i = 0; i < ini->n; i++) {
		const char *sec = ini->e[i].section, *name, *v;
		struct cg_client_cfg *k;
		char e[256];
		int n;

		if (!strcmp(sec, "client")) {
			snprintf(err, errlen, "[client]: expected [client NAME]");
			return -1;
		}
		if (strncmp(sec, "client ", 7))
			continue;
		name = sec + 7;
		if (cg_config_client(c, name))
			continue;
		if (!valid_client_name(name)) {
			snprintf(err, errlen, "[%s]: a client's name is 1 to %d lowercase letters, digits or '-', not "
				 "starting with '-' and not \"default\"", sec, CG_CLIENT_NAME_MAX);
			return -1;
		}
		if (c->nclients == CG_MAX_CLIENTS) {
			snprintf(err, errlen, "too many [client] sections (max %d)", CG_MAX_CLIENTS);
			return -1;
		}
		k = &c->clients[c->nclients];
		strcpy(k->name, name);
		k->enabled = k->passthrough = 1;
		v = cg_ini_get(ini, sec, "key");
		if (!v || cg_base64_decode(k->key, sizeof(k->key), v) != CG_KEY_LEN) {
			snprintf(err, errlen, "[%s] key: expected the base64 of %d bytes", sec, CG_KEY_LEN);
			return -1;
		}
		for (int j = 0; j < c->nclients; j++)
			if (!memcmp(c->clients[j].key, k->key, CG_KEY_LEN)) {
				snprintf(err, errlen, "[%s] key: the same as [client %s]'s", sec, c->clients[j].name);
				return -1;
			}
		if (get_str(ini, sec, "label", k->label, sizeof(k->label), e, sizeof(e)) ||
		    get_bool(ini, sec, "enabled", &k->enabled, e, sizeof(e)) ||
		    get_bool(ini, sec, "passthrough", &k->passthrough, e, sizeof(e)) ||
		    get_addrs(ini, sec, "wireguard", 0, &k->wireguard, 1, 1, &n, e, sizeof(e)) ||
		    (cg_ini_get(ini, sec, "wireguard_poke") && strcmp(cg_ini_get(ini, sec, "wireguard_poke"), "none") &&
		     get_addrs(ini, sec, "wireguard_poke", 1, &k->wireguard_poke, 1, 1, &n, e, sizeof(e)))) {
			snprintf(err, errlen, "[%s] %s", sec, e);
			return -1;
		}
		if (k->wireguard.ss_family == AF_UNSPEC) {
			if (c->wireguard.ss_family == AF_UNSPEC) {
				snprintf(err, errlen, "[%s] wireguard: required (no global one)", sec);
				return -1;
			}
			k->wireguard = c->wireguard;
		}
		v = cg_ini_get(ini, sec, "forward");
		if (v && cg_fwd_parse(v, (uint8_t)c->nclients, c->forward, &c->nforward, CG_FWD_MAX, e, sizeof(e)) < 0) {
			snprintf(err, errlen, "[%s] forward: %s", sec, e);
			return -1;
		}
		c->nclients++;
	}
	/* A port forwarded to two clients, or twice to one, is a mistake. */
	if (cg_fwd_overlap(c->forward, c->nforward, &fi, &fj)) {
		const struct cg_fwd_rule *a = &c->forward[fi], *b = &c->forward[fj];

		snprintf(err, errlen, "forward: %s %u-%u of [client %s] overlaps %s %u-%u of [client %s]",
			 proto_name(a->proto), a->first, a->last, c->clients[a->client].name, proto_name(b->proto), b->first,
			 b->last, c->clients[b->client].name);
		return -1;
	}
	return 0;
}

static int parse_lists(struct cg_config *c, struct cg_ini *ini)
{
	const char *inc = cg_ini_get(ini, "", "interfaces"), *exc = cg_ini_get(ini, "", "exclude");
	size_t len;
	char *p;

	if (!inc)
		inc = "*";
	if (!exc)
		exc = "";
	len = strlen(inc) + 1 + sizeof(default_exclude) + strlen(exc) + 1;
	c->strings = malloc(len);
	if (!c->strings)
		return -1;
	p = c->strings;
	strcpy(p, inc);
	c->ninclude = cg_split_list(p, c->include, CG_MAX_PATTERNS);
	p += strlen(inc) + 1;
	snprintf(p, len - (size_t)(p - c->strings), "%s %s", default_exclude, exc);
	c->nexclude = cg_split_list(p, c->exclude, CG_MAX_PATTERNS);
	return 0;
}

/* split: off, on or force in a client; off, client or force in a server
 * (enum cg_split_mode). c->mode is set. */
static int get_split(struct cg_ini *ini, struct cg_config *c, char *err, size_t errlen)
{
	const char *v = cg_ini_get(ini, "", "split");
	int mid = c->mode == CG_MODE_CLIENT ? CG_SPLIT_ON : CG_SPLIT_CLIENT;

	if (!v)
		return 0;
	if (!strcmp(v, "off"))
		c->split = CG_SPLIT_OFF;
	else if (!strcmp(v, cg_split_name(mid)))
		c->split = mid;
	else if (!strcmp(v, "force"))
		c->split = CG_SPLIT_FORCE;
	else {
		snprintf(err, errlen, "split: expected off, %s or force, got '%s'", cg_split_name(mid), v);
		return -1;
	}
	return 0;
}

int cg_config_parse(struct cg_config *c, const char *text, char *err, size_t errlen, char *warn,
		    size_t warnlen)
{
	struct cg_ini ini;
	struct hosts hosts = { 0, NULL };
	const char *v;
	char e[256] = "";
	int n, rc = -1, multi;
	uint32_t u;

	memset(c, 0, sizeof(*c));
	if (warnlen)
		warn[0] = '\0';
	if (cg_ini_parse(&ini, text, err, errlen) < 0)
		return -1;

	v = cg_ini_get(&ini, "", "mode");
	if (v && !strcmp(v, "client"))
		c->mode = CG_MODE_CLIENT;
	else if (v && !strcmp(v, "server"))
		c->mode = CG_MODE_SERVER;
	else {
		snprintf(err, errlen, "mode: expected 'client' or 'server'");
		goto out;
	}

	/* A server with [client] sections has a key in each, and none of its
	 * own: which client a global one would be is anybody's guess. */
	multi = c->mode == CG_MODE_SERVER && has_clients(&ini);
	v = cg_ini_get(&ini, "", "key");
	if (multi && v) {
		snprintf(err, errlen, "key: not with [client] sections, each has its own");
		goto out;
	}
	if (!multi && (!v || cg_base64_decode(c->key, sizeof(c->key), v) != CG_KEY_LEN)) {
		snprintf(err, errlen, "key: expected the base64 of %d bytes (see 'cengarde genkey')", CG_KEY_LEN);
		goto out;
	}

	c->status_interval_ms = 1000;
	c->rcvbuf = 4 << 20;
	c->log_level = CG_LOG_INFO;
	c->mute_behind_ms = 150;
	c->mute_settle_ms = 2000;
	c->min_active_links = 2;
	c->reorder = 1;
	c->reorder_max_wait_ms = 200;
	c->reorder_margin_ms = 10;
	c->reorder_bypass = 128;
	c->reorder_pool_kb = 8192;
	c->split = c->mode == CG_MODE_SERVER ? CG_SPLIT_CLIENT : CG_SPLIT_OFF;
	c->split_small = 256;
	c->cpu = -1;
	c->probe_interval_ms = 100;
	c->probe_idle_ms = 1000;
	c->max_sessions = 64;
	c->lanes = CG_LANES_AUTO;
	c->session_timeout_ms = 180000;
	c->path_timeout_ms = 30000;

	if (get_str(&ini, "", "description", c->description, sizeof(c->description), err, errlen) ||
	    get_str(&ini, "", "status_file", c->status_file, sizeof(c->status_file), err, errlen) ||
	    get_u32(&ini, "", "status_interval_ms", 100, 3600000, &c->status_interval_ms, err, errlen) ||
	    get_str(&ini, "", "control_socket", c->control_socket, sizeof(c->control_socket), err, errlen))
		goto out;
	if (c->control_socket[0] && c->control_socket[0] != '/') {
		snprintf(err, errlen, "control_socket: expected an absolute path");
		goto out;
	}
	u = (uint32_t)c->rcvbuf;
	if (get_u32(&ini, "", "rcvbuf", 0, 1 << 30, &u, err, errlen))
		goto out;
	c->rcvbuf = (int)u;
	v = cg_ini_get(&ini, "", "log_level");
	if (v && (c->log_level = cg_log_level_parse(v)) < 0) {
		snprintf(err, errlen, "log_level: expected error, warn, info or debug");
		goto out;
	}

	if (get_u32(&ini, "", "mute_behind_ms", 0, 60000, &c->mute_behind_ms, err, errlen))
		goto out;
	c->unmute_behind_ms = c->mute_behind_ms * 4 / 5;
	if (get_u32(&ini, "", "unmute_behind_ms", 0, c->mute_behind_ms, &c->unmute_behind_ms, err, errlen) ||
	    get_u32(&ini, "", "mute_settle_ms", 100, 600000, &c->mute_settle_ms, err, errlen) ||
	    get_u32(&ini, "", "mute_trickle", 0, 1000000, &c->mute_trickle, err, errlen) ||
	    get_u32(&ini, "", "min_active_links", 1, CG_MAX_LINKS, &c->min_active_links, err, errlen) ||
	    get_u32(&ini, "", "busy_poll_us", 0, 1000000, &c->busy_poll_us, err, errlen) ||
	    get_u32(&ini, "", "rt_priority", 0, 99, &c->rt_priority, err, errlen))
		goto out;
	/* Reordering and splitting, both ends (story 012): each end reorders
	 * what it receives and splits what it sends with them.
	 * reorder_dup_wait_ms cannot pass reorder_max_wait_ms. */
	if (get_bool(&ini, "", "reorder", &c->reorder, err, errlen) ||
	    get_u32(&ini, "", "reorder_max_wait_ms", 0, 2000, &c->reorder_max_wait_ms, err, errlen) ||
	    get_u32(&ini, "", "reorder_dup_wait_ms", 0, c->reorder_max_wait_ms, &c->reorder_dup_wait_ms, err,
		    errlen) ||
	    get_u32(&ini, "", "reorder_margin_ms", 0, 1000, &c->reorder_margin_ms, err, errlen) ||
	    get_u32(&ini, "", "reorder_bypass", 0, 2048, &c->reorder_bypass, err, errlen) ||
	    get_u32(&ini, "", "reorder_pool_kb", 256, 131070, &c->reorder_pool_kb, err, errlen) ||
	    get_split(&ini, c, err, errlen) ||
	    get_u32(&ini, "", "split_small", 0, 2048, &c->split_small, err, errlen))
		goto out;
	if (cg_ini_get(&ini, "", "cpu")) {
		u = 0;
		if (get_u32(&ini, "", "cpu", 0, 1023, &u, err, errlen))
			goto out;
		c->cpu = (int)u;
	}

	if (!cg_ini_get(&ini, "", "listen")) {
		cg_addr_parse(c->mode == CG_MODE_CLIENT ? "127.0.0.1:59401" : "*:59402", 0, &c->listen, 1, e,
			      sizeof(e));
	} else if (get_addrs(&ini, "", "listen", 0, &c->listen, 1, 1, &n, err, errlen)) {
		goto out;
	}

	if (c->mode == CG_MODE_CLIENT) {
		if (get_servers(&ini, "", c, &hosts, c->srv, CG_MAX_SERVERS, &c->nsrv, err, errlen) ||
		    get_resolvers(&ini, "", default_resolvers, c->resolver, &c->nresolver, err, errlen))
			goto out;
		if (!c->nsrv) {
			snprintf(err, errlen, "server: required in client mode");
			goto out;
		}
		if (get_u32(&ini, "", "probe_interval_ms", 100, 60000, &c->probe_interval_ms, err, errlen))
			goto out;
		if (c->probe_idle_ms < c->probe_interval_ms)
			c->probe_idle_ms = c->probe_interval_ms;
		if (get_u32(&ini, "", "probe_idle_ms", c->probe_interval_ms, 600000, &c->probe_idle_ms, err, errlen))
			goto out;
		/* At least 3 idle probes go unanswered before a link gives up on
		 * an address. */
		c->server_failover_ms = 10000 > 3 * c->probe_idle_ms ? 10000 : 3 * c->probe_idle_ms;
		if (get_u32(&ini, "", "server_failover_ms", 0, 3600000, &c->server_failover_ms, err, errlen))
			goto out;
		if (c->server_failover_ms && c->server_failover_ms < 3 * c->probe_idle_ms) {
			snprintf(err, errlen, "server_failover_ms: expected 0 (never) or at least 3 x probe_idle_ms (%u)",
				 3 * c->probe_idle_ms);
			goto out;
		}
		u = 0;
		if (get_u32(&ini, "", "sndbuf", 0, 1 << 30, &u, err, errlen))
			goto out;
		c->sndbuf = (int)u;
		c->passthrough = -1;
		if (get_bool(&ini, "", "passthrough", &c->passthrough, err, errlen))
			goto out;
		/* legacy, the loop of 0.4, until the threads are measured on the
		 * Pi (thrplan.h: auto is legacy in this release too). */
		c->link_threads = CG_LT_LEGACY;
		v = cg_ini_get(&ini, "", "link_threads");
		if (v) {
			for (c->link_threads = CG_LT_AUTO; c->link_threads <= CG_LT_LEGACY; c->link_threads++)
				if (!strcmp(v, cg_lt_name(c->link_threads)))
					break;
			if (c->link_threads > CG_LT_LEGACY) {
				snprintf(err, errlen, "link_threads: expected auto, on, off or legacy, got '%s'", v);
				goto out;
			}
		}
		c->io_queue = 256;
		if (get_u32(&ini, "", "io_queue", 64, 1024, &c->io_queue, err, errlen))
			goto out;
		if (c->io_queue & (c->io_queue - 1)) {
			snprintf(err, errlen, "io_queue: expected a power of two between 64 and 1024, got %u", c->io_queue);
			goto out;
		}
		if (parse_lists(c, &ini) < 0) {
			snprintf(err, errlen, "out of memory");
			goto out;
		}
		if (parse_links(c, &ini, &hosts, err, errlen))
			goto out;
		cg_config_build(c, NULL);
	} else {
		if (get_addrs(&ini, "", "wireguard", 0, &c->wireguard, 1, 1, &n, err, errlen))
			goto out;
		if (!n && !multi) {
			snprintf(err, errlen, "wireguard: required in server mode (local WireGuard address)");
			goto out;
		}
		if (multi && parse_clients(c, &ini, err, errlen))
			goto out;
		/* Room for every client's sessions (CG_CLIENT_SESSIONS each). */
		if (c->max_sessions < (uint32_t)c->nclients * CG_CLIENT_SESSIONS)
			c->max_sessions = (uint32_t)c->nclients * CG_CLIENT_SESSIONS;
		if (get_u32(&ini, "", "max_sessions", 1, 4096, &c->max_sessions, err, errlen) ||
		    get_u32(&ini, "", "session_timeout_ms", 1000, 86400000, &c->session_timeout_ms, err, errlen) ||
		    get_u32(&ini, "", "path_timeout_ms", 1000, 3600000, &c->path_timeout_ms, err, errlen) ||
		    get_str(&ini, "", "passthrough_file", c->passthrough_file, sizeof(c->passthrough_file), err,
			    errlen) ||
		    get_str(&ini, "", "forward_file", c->forward_file, sizeof(c->forward_file), err, errlen))
			goto out;
		/* One client asks for IP pass in passthrough_file; several share
		 * the forward table. */
		if (multi && c->passthrough_file[0]) {
			snprintf(err, errlen, "passthrough_file: not with [client] sections, see forward_file");
			goto out;
		}
		if (!multi && c->forward_file[0]) {
			snprintf(err, errlen, "forward_file: only with [client] sections (one client: passthrough_file)");
			goto out;
		}
		if (multi && cg_ini_get(&ini, "", "wireguard_poke")) {
			snprintf(err, errlen, "wireguard_poke: with [client] sections, in each of them (that client's "
				 "address in the tunnel)");
			goto out;
		}
		v = cg_ini_get(&ini, "", "lanes");
		if (v && strcmp(v, "auto")) {
			char *end;
			unsigned long l;

			errno = 0;
			l = strtoul(v, &end, 10);
			if (!*v || *end || errno || !cg_lanes_valid((unsigned)l) || l != (unsigned)l) {
				snprintf(err, errlen, "lanes: expected auto, 1, 2, 4, 8 or 16, got '%s'", v);
				goto out;
			}
			c->lanes = (uint32_t)l;
		}
		/* The router's end of the tunnel cengarde sets up (10.79.0.2/30). */
		v = cg_ini_get(&ini, "", "wireguard_poke");
		if (multi)
			;
		else if (!v)
			cg_addr_parse("10.79.0.2:9", 0, &c->wireguard_poke, 1, e, sizeof(e));
		else if (strcmp(v, "none") &&
			 get_addrs(&ini, "", "wireguard_poke", 1, &c->wireguard_poke, 1, 1, &n, err, errlen))
			goto out;
	}

	for (int i = 0; i < ini.n; i++) {
		size_t used;

		if (ini.e[i].used || !warnlen)
			continue;
		used = strlen(warn);
		snprintf(warn + used, warnlen - used, "%sline %d: unknown key '%s'%s%s%s", used ? "\n" : "",
			 ini.e[i].line, ini.e[i].key, *ini.e[i].section ? " in [" : "", ini.e[i].section,
			 *ini.e[i].section ? "]" : "");
	}
	rc = 0;
out:
	cg_ini_free(&ini);
	free(hosts.text);
	if (rc)
		cg_config_free(c);
	return rc;
}

static char *read_file(const char *path, char *err, size_t errlen)
{
	FILE *f = fopen(path, "r");
	char *text;
	long len;

	if (!f) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || len > (1 << 20) || fseek(f, 0, SEEK_SET)) {
		snprintf(err, errlen, "%s: cannot read (or larger than 1 MiB)", path);
		fclose(f);
		return NULL;
	}
	text = malloc((size_t)len + 1);
	if (!text || fread(text, 1, (size_t)len, f) != (size_t)len) {
		snprintf(err, errlen, "%s: read error", path);
		free(text);
		fclose(f);
		return NULL;
	}
	text[len] = '\0';
	fclose(f);
	return text;
}

int cg_config_load(struct cg_config *c, const char *path, char *err, size_t errlen, char *warn,
		   size_t warnlen)
{
	char *text = read_file(path, err, errlen), e[384];
	int rc;

	if (!text)
		return -1;
	rc = cg_config_parse(c, text, e, sizeof(e), warn, warnlen);
	if (rc < 0)
		snprintf(err, errlen, "%s: %s", path, e);
	free(text);
	return rc;
}

int cg_config_peek(const char *path, const char *key, char *out, size_t outlen, char *err, size_t errlen)
{
	char *text = read_file(path, err, errlen);
	struct cg_ini ini;
	const char *v;
	int rc = 1;

	if (!text)
		return -1;
	if (cg_ini_parse(&ini, text, err, errlen) < 0) {
		free(text);
		return -1;
	}
	free(text);
	v = cg_ini_get(&ini, "", key);
	if (v && strlen(v) >= outlen) {
		snprintf(err, errlen, "%s: value too long", key);
		rc = -1;
	} else if (v) {
		strcpy(out, v);
		rc = 0;
	}
	cg_ini_free(&ini);
	return rc;
}

/* Whether a [link] pins its thread elsewhere: each pump is pinned once. */
static int links_cpu_differ(const struct cg_config *a, const struct cg_config *b)
{
	for (int i = 0; i < a->nlinks; i++) {
		const struct cg_link_cfg *o = cg_config_link(b, a->links[i].name);

		if (a->links[i].cpu != (o ? o->cpu : -1))
			return 1;
	}
	for (int i = 0; i < b->nlinks; i++)
		if (b->links[i].cpu >= 0 && !cg_config_link(a, b->links[i].name))
			return 1;
	return 0;
}

const char *cg_config_restart_needed(const struct cg_config *a, const struct cg_config *b)
{
	if (a->mode != b->mode)
		return "mode";
	/* One client and several are different servers; within either, the
	 * clients change in place (server.c). */
	if (a->mode == CG_MODE_SERVER && !a->nclients != !b->nclients)
		return "[client] sections";
	if (memcmp(a->key, b->key, sizeof(a->key)))
		return "key";
	if (!cg_addr_equal(&a->listen, &b->listen))
		return "listen";
	if (strcmp(a->control_socket, b->control_socket))
		return "control_socket";
	if (a->busy_poll_us != b->busy_poll_us)
		return "busy_poll_us";
	if (a->cpu != b->cpu)
		return "cpu";
	if (a->rt_priority != b->rt_priority)
		return "rt_priority";
	if (a->reorder_pool_kb != b->reorder_pool_kb)
		return "reorder_pool_kb"; /* the pool is allocated once */
	if (a->mode == CG_MODE_CLIENT && a->link_threads != b->link_threads)
		return "link_threads";
	if (a->mode == CG_MODE_CLIENT && a->io_queue != b->io_queue)
		return "io_queue";
	if (a->mode == CG_MODE_CLIENT && links_cpu_differ(a, b))
		return "cpu of a [link]";
	if (a->mode == CG_MODE_SERVER && !a->nclients && !cg_addr_equal(&a->wireguard, &b->wireguard))
		return "wireguard";
	if (a->mode == CG_MODE_SERVER && a->lanes != b->lanes)
		return "lanes";
	return NULL;
}

void cg_config_free(struct cg_config *c)
{
	free(c->strings);
	c->strings = NULL;
	c->ninclude = c->nexclude = 0;
	free(c->clients);
	c->clients = NULL;
	c->nclients = 0;
	free(c->forward);
	c->forward = NULL;
	c->nforward = 0;
}
