/* The live class (livesni.h): the name of a TLS ClientHello, the ingests,
 * RTMP's port.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "livesni.h"
#include "test.h"

/* A ClientHello asking for name (NULL: no server_name), after another
 * extension; its length. */
static size_t hello(uint8_t *b, const char *name)
{
	size_t i = 0, ext, hs, nl = name ? strlen(name) : 0;

	b[i++] = 0x16, b[i++] = 0x03, b[i++] = 0x01, i += 2; /* the record, its length later */
	b[i++] = 0x01, i += 3;                               /* ClientHello, its length later */
	hs = i;
	b[i++] = 0x03, b[i++] = 0x03;
	memset(b + i, 0x5a, 32), i += 32;           /* random */
	b[i++] = 32, memset(b + i, 7, 32), i += 32; /* a session id */
	b[i++] = 0, b[i++] = 4, b[i++] = 0x13, b[i++] = 0x01, b[i++] = 0x13, b[i++] = 0x02;
	b[i++] = 1, b[i++] = 0; /* compression: null */
	ext = i, i += 2;
	b[i++] = 0x00, b[i++] = 0x0b, b[i++] = 0, b[i++] = 2, b[i++] = 1, b[i++] = 0; /* ec_point_formats */
	if (name) {
		b[i++] = 0, b[i++] = 0, b[i++] = (uint8_t)((nl + 5) >> 8), b[i++] = (uint8_t)(nl + 5);
		b[i++] = (uint8_t)((nl + 3) >> 8), b[i++] = (uint8_t)(nl + 3);
		b[i++] = 0, b[i++] = (uint8_t)(nl >> 8), b[i++] = (uint8_t)nl;
		memcpy(b + i, name, nl), i += nl;
	}
	b[ext] = (uint8_t)((i - ext - 2) >> 8), b[ext + 1] = (uint8_t)(i - ext - 2);
	b[hs - 3] = (uint8_t)((i - hs) >> 16), b[hs - 2] = (uint8_t)((i - hs) >> 8), b[hs - 1] = (uint8_t)(i - hs);
	b[3] = (uint8_t)((i - 5) >> 8), b[4] = (uint8_t)(i - 5);
	return i;
}

void test_livesni(void)
{
	uint8_t b[600];
	char host[CG_SNI_MAX + 1];
	size_t n;

	n = hello(b, "a.rtmps.YouTube.com");
	CHECK(cg_tls_sni(b, n, host, sizeof(host)) && !strcmp(host, "a.rtmps.youtube.com"));
	CHECK(cg_live_flow(443, b, n));
	CHECK(!cg_tls_sni(b, n - 3, host, sizeof(host))); /* not all there yet */
	CHECK(!cg_tls_sni(b, n, host, 8));                /* more than fits */
	n = hello(b, "www.youtube.com");
	CHECK(cg_tls_sni(b, n, host, sizeof(host)) && !cg_live_flow(443, b, n));
	n = hello(b, NULL);
	CHECK(!cg_tls_sni(b, n, host, sizeof(host)));
	b[5] = 0x02; /* a ServerHello */
	CHECK(!cg_tls_sni(b, n, host, sizeof(host)));
	CHECK(cg_live_flow(1935, (const uint8_t *)"\x03", 1)); /* RTMP's handshake: its port */
	CHECK(!cg_live_flow(80, (const uint8_t *)"GET / HTTP/1.1\r\n", 16));

	CHECK(cg_live_host("live-api-s.facebook.com"));
	CHECK(cg_live_host("sao03.contribute.live-video.net"));
	CHECK(cg_live_host("live.restream.io"));
	CHECK(cg_live_host("restream.io"));
	CHECK(!cg_live_host("notrestream.io"));  /* label by label */
	CHECK(!cg_live_host("www.facebook.com"));
	CHECK(!cg_live_host("live-video.net.example.com"));
}
