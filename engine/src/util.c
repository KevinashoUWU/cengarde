/* SPDX-License-Identifier: GPL-2.0-only */
#include "util.h"

#include <arpa/inet.h>
#include <fnmatch.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void cg_base64_encode(char *out, const uint8_t *in, size_t len)
{
	size_t i;

	for (i = 0; i + 2 < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];

		*out++ = b64[v >> 18];
		*out++ = b64[(v >> 12) & 63];
		*out++ = b64[(v >> 6) & 63];
		*out++ = b64[v & 63];
	}
	if (i < len) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0);

		*out++ = b64[v >> 18];
		*out++ = b64[(v >> 12) & 63];
		*out++ = i + 1 < len ? b64[(v >> 6) & 63] : '=';
		*out++ = '=';
	}
	*out = '\0';
}

static int b64_val(char c)
{
	const char *p = c ? strchr(b64, c) : NULL;

	return p ? (int)(p - b64) : -1;
}

int cg_base64_decode(uint8_t *out, size_t outcap, const char *in)
{
	size_t len = strlen(in), n = 0;

	if (len % 4)
		return -1;
	for (size_t i = 0; i < len; i += 4) {
		int v[4], pad = 0;

		for (int j = 0; j < 4; j++) {
			if (in[i + j] == '=' && i + 4 == len && j >= 2) {
				v[j] = 0;
				pad++;
			} else if (pad || (v[j] = b64_val(in[i + j])) < 0) {
				return -1;
			}
		}
		uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
		int bytes = 3 - pad;

		if (n + (size_t)bytes > outcap)
			return -1;
		out[n++] = (uint8_t)(w >> 16);
		if (bytes > 1)
			out[n++] = (uint8_t)(w >> 8);
		if (bytes > 2)
			out[n++] = (uint8_t)w;
	}
	return (int)n;
}

static int parse_port(const char *s, uint16_t *port)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (!*s || *end || v < 1 || v > 65535)
		return -1;
	*port = (uint16_t)v;
	return 0;
}

int cg_addr_parse(const char *s, int allow_names, struct sockaddr_storage *out, int max, char *err,
		  size_t errlen)
{
	char host[256];
	const char *colon;
	uint16_t port;
	size_t hlen;

	if (s[0] == '[') {
		const char *close = strchr(s, ']');

		if (!close || close[1] != ':') {
			snprintf(err, errlen, "malformed address '%s' (expected [ipv6]:port)", s);
			return -1;
		}
		hlen = (size_t)(close - s - 1);
		memcpy(host, s + 1, hlen < sizeof(host) ? hlen : 0);
		colon = close + 1;
	} else {
		colon = strrchr(s, ':');
		if (!colon || strchr(s, ':') != colon) {
			snprintf(err, errlen, "malformed address '%s' (expected host:port)", s);
			return -1;
		}
		hlen = (size_t)(colon - s);
		memcpy(host, s, hlen < sizeof(host) ? hlen : 0);
	}
	if (hlen == 0 || hlen >= sizeof(host)) {
		snprintf(err, errlen, "malformed address '%s'", s);
		return -1;
	}
	host[hlen] = '\0';
	if (parse_port(colon + 1, &port) < 0) {
		snprintf(err, errlen, "bad port in '%s'", s);
		return -1;
	}

	memset(out, 0, sizeof(*out) * (size_t)max);
	if (strcmp(host, "*") == 0) {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)out;

		a->sin6_family = AF_INET6;
		a->sin6_addr = in6addr_any;
		a->sin6_port = htons(port);
		return 1;
	}
	{
		struct sockaddr_in *a4 = (struct sockaddr_in *)out;
		struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)out;

		if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) {
			a4->sin_family = AF_INET;
			a4->sin_port = htons(port);
			return 1;
		}
		if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) {
			a6->sin6_family = AF_INET6;
			a6->sin6_port = htons(port);
			return 1;
		}
	}
	if (!allow_names) {
		snprintf(err, errlen, "'%s' is not a numeric address", host);
		return -1;
	}

	struct addrinfo hints = { .ai_socktype = SOCK_DGRAM }, *res, *ai;
	int rc = getaddrinfo(host, NULL, &hints, &res), n = 0;

	if (rc) {
		snprintf(err, errlen, "cannot resolve '%s': %s", host, gai_strerror(rc));
		return -1;
	}
	for (ai = res; ai && n < max; ai = ai->ai_next) {
		if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
			continue;
		memcpy(&out[n], ai->ai_addr, ai->ai_addrlen);
		if (ai->ai_family == AF_INET)
			((struct sockaddr_in *)&out[n])->sin_port = htons(port);
		else
			((struct sockaddr_in6 *)&out[n])->sin6_port = htons(port);
		n++;
	}
	freeaddrinfo(res);
	if (!n)
		snprintf(err, errlen, "'%s' has no IPv4 or IPv6 address", host);
	return n ? n : -1;
}

const char *cg_addr_str(const struct sockaddr_storage *a, char *buf, size_t len)
{
	char ip[INET6_ADDRSTRLEN];

	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;

		inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
		snprintf(buf, len, "%s:%u", ip, ntohs(a4->sin_port));
	} else if (a->ss_family == AF_INET6) {
		const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;

		if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
			inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], ip, sizeof(ip));
			snprintf(buf, len, "%s:%u", ip, ntohs(a6->sin6_port));
		} else {
			inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
			snprintf(buf, len, "[%s]:%u", ip, ntohs(a6->sin6_port));
		}
	} else {
		snprintf(buf, len, "?");
	}
	return buf;
}

socklen_t cg_addr_len(const struct sockaddr_storage *a)
{
	return a->ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
}

int cg_addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family)
		return 0;
	if (a->ss_family == AF_INET) {
		const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;

		return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
	}
	if (a->ss_family == AF_INET6) {
		const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;

		return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, 16);
	}
	return 0;
}

int cg_split_list(char *s, char **items, int max)
{
	int n = 0;
	char *save = NULL, *tok;

	for (tok = strtok_r(s, " \t,", &save); tok && n < max; tok = strtok_r(NULL, " \t,", &save))
		items[n++] = tok;
	return n;
}

int cg_match_any(const char *name, char *const *patterns, int npatterns)
{
	for (int i = 0; i < npatterns; i++)
		if (fnmatch(patterns[i], name, 0) == 0)
			return 1;
	return 0;
}
