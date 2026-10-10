/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_UTIL_H
#define CG_UTIL_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

#define CG_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static inline uint64_t cg_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static inline uint64_t cg_now_ms(void)
{
	return cg_now_us() / 1000u;
}

/* Wall-clock time in milliseconds since the epoch, for the status file only
 * (a router without an RTC jumps when NTP sets it): timers use cg_now_ms. */
static inline uint64_t cg_wall_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* How much a 32-bit counter grew from last to now, across its wrap: counters
 * read across threads are 32-bit (MIPS32 has no 64-bit atomics), and their
 * reader keeps the 64-bit totals. */
static inline uint32_t cg_delta32(uint32_t now, uint32_t last)
{
	return now - last;
}

/* Adds the growth of a 32-bit counter since *last to *total. */
static inline void cg_acc32(uint64_t *total, uint32_t *last, uint32_t now)
{
	*total += cg_delta32(now, *last);
	*last = now;
}

/* Standard base64 with padding. encode: out must hold 4*ceil(len/3)+1 bytes.
 * decode: returns the decoded length, or -1 on malformed input or overflow. */
void cg_base64_encode(char *out, const uint8_t *in, size_t len);
int cg_base64_decode(uint8_t *out, size_t outcap, const char *in);

/* "1.2.3.4:5", "[2001:db8::1]:5", "*:5" (IPv6 any, dual-stack) or, when
 * allow_names is set, "host.example:5". Fills up to max addresses and returns
 * how many, or -1 with a message in err. IPv4-mapped addresses come back as
 * IPv4 (cg_addr_unmap). */
int cg_addr_parse(const char *s, int allow_names, struct sockaddr_storage *out, int max, char *err,
		  size_t errlen);

/* Turns an IPv4-mapped IPv6 address ("[::ffff:192.0.2.1]:5") into plain
 * IPv4, port included, so that it goes out of an IPv4 socket. Leaves any
 * other address alone. */
void cg_addr_unmap(struct sockaddr_storage *a);

/* Why a cannot be a peer to send to ("the wildcard address", "a multicast
 * address"), or NULL when it can. IPv4-mapped addresses count as IPv4. */
const char *cg_addr_unfit_peer(const struct sockaddr_storage *a);

/* Writes "1.2.3.4:5" or "[2001:db8::1]:5"; IPv4-mapped IPv6 prints as IPv4. */
const char *cg_addr_str(const struct sockaddr_storage *a, char *buf, size_t len);

socklen_t cg_addr_len(const struct sockaddr_storage *a);
int cg_addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b);

/* Splits a whitespace or comma separated list in place. Returns the count. */
int cg_split_list(char *s, char **items, int max);

/* Does name match any fnmatch(3) pattern in the list? */
int cg_match_any(const char *name, char *const *patterns, int npatterns);

/* At most one event per interval; counts what it suppressed. */
struct cg_ratelimit {
	uint64_t last_ms;
	uint32_t suppressed;
};

static inline int cg_ratelimit_ok(struct cg_ratelimit *rl, uint64_t now_ms, uint64_t interval_ms)
{
	if (rl->last_ms && now_ms - rl->last_ms < interval_ms) {
		rl->suppressed++;
		return 0;
	}
	rl->last_ms = now_ms ? now_ms : 1;
	return 1;
}

#endif
