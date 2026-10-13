/* Server side: the port WireGuard knows each client by, across restarts.
 *
 * WireGuard on the VPS knows a client by the endpoint it last heard it from:
 * the local port of the server's socket for that client's session. When the
 * client's clock went back, WireGuard ignores its handshakes, and only a
 * handshake WireGuard starts itself, sent to that endpoint, brings the
 * tunnel back (wgwatch.h). The server keeps the port in memory and hands it
 * to the next session that starts alone; a restart of the engine (an
 * upgrade, a crash, a reload that restarts it) used to lose it, and with a
 * router whose clock was behind at the same time nothing could bring the
 * tunnel back but a restart of WireGuard on the VPS.
 *
 * So the server writes every client's port to wireguard_port_file, from a
 * thread, and reads it once at start: the first session of each client
 * binds that port again. The file is data only: a header line naming the
 * format version, then one line per client,
 *
 *	PORT NAME
 *
 * PORT a decimal number from 1 to 65535 and NAME the [client] section's name
 * (lowercase letters, digits and hyphens, validated by the configuration);
 * the one client of a server without [client] sections has no name, and its
 * line is "PORT" alone. Every line ends with "\n". A line that does not
 * parse is skipped: the worst a bad file does is a session on a new port,
 * as without the file.
 *
 * No allocation, no I/O.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_WGPORT_H
#define CG_WGPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The first line; a format change bumps the version, and a file of another
 * version is not read. */
#define CG_WGPORT_VERSION "1"
#define CG_WGPORT_TITLE "# cengarde WireGuard ports "
#define CG_WGPORT_HEADER CG_WGPORT_TITLE CG_WGPORT_VERSION ", written by the engine: data only\n"

/* The file for n clients, names[i] with ports[i] (host order; 0: none, no
 * line). Returns its length, or -1 when it does not fit in size. */
static inline int cg_wgport_table(char *buf, size_t size, const char *const *names, const uint16_t *ports, int n)
{
	size_t len = sizeof(CG_WGPORT_HEADER) - 1;
	int w;

	if (!buf || size <= len)
		return -1;
	memcpy(buf, CG_WGPORT_HEADER, len + 1);
	for (int i = 0; i < n; i++) {
		if (!ports[i])
			continue;
		w = snprintf(buf + len, size - len, names[i][0] ? "%u %s\n" : "%u\n", (unsigned)ports[i], names[i]);
		if (w < 0 || (size_t)w >= size - len)
			return -1;
		len += (size_t)w;
	}
	return (int)len;
}

/* The port of the client named name (host order) in the file text, 0 when
 * it has none or the text is not such a file. The last line for a name
 * wins. */
static inline uint16_t cg_wgport_find(const char *text, const char *name)
{
	const size_t tlen = sizeof(CG_WGPORT_TITLE) - 1, vlen = sizeof(CG_WGPORT_VERSION) - 1;
	const size_t nlen = name ? strlen(name) : 0;
	const char *p;
	uint16_t found = 0;

	/* The version is the whole number: "ports 10" is not ports 1. */
	if (!text || !name || strncmp(text, CG_WGPORT_TITLE, tlen) || strncmp(text + tlen, CG_WGPORT_VERSION, vlen) ||
	    (text[tlen + vlen] >= '0' && text[tlen + vlen] <= '9'))
		return 0;
	/* p rests on the newline that ends the line just read; ++p starts the next. */
	p = strchr(text + tlen + vlen, '\n');
	while (p && *++p) {
		const char *end = strchr(p, '\n');
		uint32_t port = 0;
		size_t digits = 0;

		if (!end)
			break; /* a line cut short: the file was not written whole */
		while (p + digits < end && p[digits] >= '0' && p[digits] <= '9' && digits < 6)
			port = port * 10 + (uint32_t)(p[digits++] - '0');
		if (digits && digits < 6 && p[0] != '0' && port <= 65535) {
			const char *q = p + digits;

			if (nlen ? (end - q == (ptrdiff_t)nlen + 1 && *q == ' ' && !memcmp(q + 1, name, nlen)) : q == end)
				found = (uint16_t)port;
		}
		p = end;
	}
	return found;
}

#endif
