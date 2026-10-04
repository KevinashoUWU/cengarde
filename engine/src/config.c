/* SPDX-License-Identifier: GPL-2.0-only */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arrival.h" /* CG_MAX_LINKS */
#include "log.h"
#include "util.h"

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

/* Parses a list of at most max entries into out; a name may stand for several
 * addresses and takes the slots the later entries leave, so every entry is
 * parsed, checked and kept. Peers are addresses to send to: names are allowed
 * there, the wildcard and multicast addresses are not. */
static int get_addrs(struct cg_ini *ini, const char *sec, const char *key, int peers,
		     struct sockaddr_storage *out, int max, int *n, char *err, size_t errlen)
{
	const char *v = cg_ini_get(ini, sec, key);
	char buf[512], *items[CG_MAX_SERVER_ADDRS * 2], e[256];
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
	for (int i = 0; i < count; i++) { /* count <= max: a slot is left for each later entry */
		int got = cg_addr_parse(items[i], peers, out + *n, max - *n - (count - 1 - i), e, sizeof(e));
		const char *why = NULL;

		if (got < 0) {
			snprintf(err, errlen, "%s: %s", key, e);
			return -1;
		}
		for (int j = 0; peers && j < got && !why; j++)
			why = cg_addr_unfit_peer(&out[*n + j]);
		if (why) {
			snprintf(err, errlen, "%s: '%s' is %s", key, items[i], why);
			return -1;
		}
		*n += got;
	}
	if (!*n) {
		snprintf(err, errlen, "%s: no address given", key);
		return -1;
	}
	return 0;
}

const struct cg_link_cfg *cg_config_link(const struct cg_config *c, const char *ifname)
{
	for (int i = 0; i < c->nlinks; i++)
		if (!strcmp(c->links[i].name, ifname))
			return &c->links[i];
	return NULL;
}

static int parse_links(struct cg_config *c, struct cg_ini *ini, char *err, size_t errlen)
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
		if (get_str(ini, sec, "label", l->label, sizeof(l->label), err, errlen) ||
		    get_bool(ini, sec, "enabled", &l->enabled, err, errlen) ||
		    get_addrs(ini, sec, "server", 1, l->server, CG_MAX_SERVER_ADDRS, &l->nserver, err, errlen))
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

int cg_config_parse(struct cg_config *c, const char *text, char *err, size_t errlen, char *warn,
		    size_t warnlen)
{
	struct cg_ini ini;
	const char *v;
	char e[256] = "";
	int n, rc = -1;
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

	v = cg_ini_get(&ini, "", "key");
	if (!v || cg_base64_decode(c->key, sizeof(c->key), v) != CG_KEY_LEN) {
		snprintf(err, errlen, "key: expected the base64 of %d bytes (see 'cengarde genkey')", CG_KEY_LEN);
		goto out;
	}

	c->status_interval_ms = 1000;
	c->rcvbuf = 4 << 20;
	c->log_level = CG_LOG_INFO;
	c->mute_behind_ms = 150;
	c->mute_settle_ms = 2000;
	c->min_active_links = 2;
	c->cpu = -1;
	c->probe_interval_ms = 100;
	c->probe_idle_ms = 1000;
	c->max_sessions = 64;
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
	if (cg_ini_get(&ini, "", "cpu")) {
		u = 0;
		if (get_u32(&ini, "", "cpu", 0, 1023, &u, err, errlen))
			goto out;
		c->cpu = (int)u;
	}

	if (!cg_ini_get(&ini, "", "listen")) {
		cg_addr_parse(c->mode == CG_MODE_CLIENT ? "127.0.0.1:59401" : "*:59402", 0, &c->listen, 1, e,
			      sizeof(e));
	} else if (get_addrs(&ini, "", "listen", 0, &c->listen, 1, &n, err, errlen)) {
		goto out;
	}

	if (c->mode == CG_MODE_CLIENT) {
		if (get_addrs(&ini, "", "server", 1, c->server, CG_MAX_SERVER_ADDRS, &c->nserver, err, errlen))
			goto out;
		if (!c->nserver) {
			snprintf(err, errlen, "server: required in client mode");
			goto out;
		}
		if (get_u32(&ini, "", "probe_interval_ms", 100, 60000, &c->probe_interval_ms, err, errlen))
			goto out;
		if (c->probe_idle_ms < c->probe_interval_ms)
			c->probe_idle_ms = c->probe_interval_ms;
		if (get_u32(&ini, "", "probe_idle_ms", c->probe_interval_ms, 600000, &c->probe_idle_ms, err, errlen))
			goto out;
		u = 0;
		if (get_u32(&ini, "", "sndbuf", 0, 1 << 30, &u, err, errlen))
			goto out;
		c->sndbuf = (int)u;
		c->passthrough = -1;
		if (get_bool(&ini, "", "passthrough", &c->passthrough, err, errlen))
			goto out;
		if (parse_lists(c, &ini) < 0) {
			snprintf(err, errlen, "out of memory");
			goto out;
		}
		if (parse_links(c, &ini, err, errlen))
			goto out;
	} else {
		if (get_addrs(&ini, "", "wireguard", 0, &c->wireguard, 1, &n, err, errlen))
			goto out;
		if (!n) {
			snprintf(err, errlen, "wireguard: required in server mode (local WireGuard address)");
			goto out;
		}
		if (get_u32(&ini, "", "max_sessions", 1, 4096, &c->max_sessions, err, errlen) ||
		    get_u32(&ini, "", "session_timeout_ms", 1000, 86400000, &c->session_timeout_ms, err, errlen) ||
		    get_u32(&ini, "", "path_timeout_ms", 1000, 3600000, &c->path_timeout_ms, err, errlen) ||
		    get_str(&ini, "", "passthrough_file", c->passthrough_file, sizeof(c->passthrough_file), err,
			    errlen))
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

const char *cg_config_restart_needed(const struct cg_config *a, const struct cg_config *b)
{
	if (a->mode != b->mode)
		return "mode";
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
	if (a->mode == CG_MODE_SERVER && !cg_addr_equal(&a->wireguard, &b->wireguard))
		return "wireguard";
	if (a->mode == CG_MODE_SERVER && a->max_sessions != b->max_sessions)
		return "max_sessions";
	return NULL;
}

void cg_config_free(struct cg_config *c)
{
	free(c->strings);
	c->strings = NULL;
	c->ninclude = c->nexclude = 0;
}
