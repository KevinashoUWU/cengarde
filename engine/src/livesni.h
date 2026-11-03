/* The live class (story 031): a TCP stream the router intercepts that
 * carries live video to an ingest server (RTMP, RTMPS: OBS, a phone, a
 * hardware encoder to YouTube, Facebook, Twitch...) goes in GRIT's video
 * tier (dscp.h's AF41) without the device marking it: before the bulk in
 * the sends while it stays under half of the pacer's rate. Two ways to
 * know one: plain RTMP's port (1935), or the name a TLS ClientHello asks
 * for (its server_name extension, in the clear in its first bytes) under
 * one of the ingests below.
 *
 * Pure: no clock, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CG_LIVESNI_H
#define CG_LIVESNI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CG_LIVE_RTMP_PORT 1935
#define CG_LIVE_DSCP 34 /* AF41: dscp.h's video tier */
#define CG_SNI_MAX 255

/* The name the TLS ClientHello in the first n bytes of a stream asks for,
 * lowercase and NUL-terminated into out (outlen >= 2): 1, or 0 when they
 * are not a ClientHello, or it has no name or more than fits, or it is not
 * all there yet (a name that comes later than the first read is not
 * looked for). */
static inline int cg_tls_sni(const uint8_t *p, size_t n, char *out, size_t outlen)
{
	size_t i, end, rec;

	if (n < 5 + 4 || p[0] != 0x16 || p[1] != 0x03 || p[5] != 0x01) /* a handshake record, a ClientHello */
		return 0;
	rec = 5 + ((size_t)p[3] << 8 | p[4]);
	end = rec < n ? rec : n;
	i = 5 + 4 + 2 + 32; /* the handshake's header, the version, the random */
	if (i + 1 > end)
		return 0;
	i += 1 + p[i]; /* the session id */
	if (i + 2 > end)
		return 0;
	i += 2 + ((size_t)p[i] << 8 | p[i + 1]); /* the cipher suites */
	if (i + 1 > end)
		return 0;
	i += 1 + p[i]; /* the compression methods */
	if (i + 2 > end)
		return 0;
	i += 2; /* the extensions' length: each one is checked against end */
	while (i + 4 <= end) {
		size_t type = (size_t)p[i] << 8 | p[i + 1], len = (size_t)p[i + 2] << 8 | p[i + 3];

		i += 4;
		if (i + len > end)
			return 0;
		if (type == 0) { /* server_name: a list, the first host_name */
			size_t j = i + 2, nl;

			if (len < 5 || p[j] != 0)
				return 0;
			nl = (size_t)p[j + 1] << 8 | p[j + 2];
			j += 3;
			if (!nl || j + nl > i + len || nl >= outlen)
				return 0;
			for (size_t k = 0; k < nl; k++) {
				uint8_t c = p[j + k];

				if (c < 0x21 || c > 0x7e)
					return 0;
				out[k] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
			}
			out[nl] = 0;
			return 1;
		}
		i += len;
	}
	return 0;
}

/* Whether host (lowercase) is a live ingest: one of these names, or a name
 * under one (label by label). */
static inline int cg_live_host(const char *host)
{
	static const char *const ingest[] = {
		"rtmp.youtube.com",          /* a.rtmp.youtube.com, b.rtmp... */
		"rtmps.youtube.com",         /* a.rtmps.youtube.com */
		"live-api-s.facebook.com",   /* Facebook Live's RTMPS */
		"live-api.facebook.com",
		"rtmp-api.facebook.com",
		"live-upload.instagram.com", /* Instagram Live */
		"live-video.net",            /* Twitch and Kick (Amazon IVS): *.contribute.live-video.net */
		"restream.io",               /* live.restream.io */
		"live.mux.com",              /* global-live.mux.com */
	};
	size_t hl = strlen(host);

	for (size_t i = 0; i < sizeof(ingest) / sizeof(ingest[0]); i++) {
		size_t il = strlen(ingest[i]);

		if (hl >= il && !strcmp(host + hl - il, ingest[i]) && (hl == il || host[hl - il - 1] == '.'))
			return 1;
	}
	return 0;
}

/* Whether a TCP stream to port whose first n bytes are p is live. */
static inline int cg_live_flow(uint16_t port, const uint8_t *p, size_t n)
{
	char host[CG_SNI_MAX + 1];

	if (port == CG_LIVE_RTMP_PORT)
		return 1;
	return cg_tls_sni(p, n, host, sizeof(host)) && cg_live_host(host);
}

#endif
