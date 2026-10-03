/* Minimal test harness: each suite is a function, CHECK records failures.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_TEST_H
#define CG_TEST_H

#include <inttypes.h>
#include <stdio.h>

extern int cg_test_failures;
extern int cg_test_checks;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		cg_test_checks++;                                                                  \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);   \
			cg_test_failures++;                                                        \
		}                                                                                  \
	} while (0)

#define CHECK_EQ(a, b)                                                                             \
	do {                                                                                       \
		long long _a = (long long)(a), _b = (long long)(b);                                \
		cg_test_checks++;                                                                  \
		if (_a != _b) {                                                                    \
			fprintf(stderr, "%s:%d: %s == %s failed (%lld != %lld)\n", __FILE__, __LINE__, \
				#a, #b, _a, _b);                                                   \
			cg_test_failures++;                                                        \
		}                                                                                  \
	} while (0)

void test_siphash(void);
void test_proto(void);
void test_replay(void);
void test_arrival(void);
void test_config(void);
void test_util(void);
void test_idmap(void);

#endif
