/* SPDX-License-Identifier: GPL-2.0-only */
#include "test.h"

int cg_test_failures;
int cg_test_checks;

int main(void)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} suites[] = {
		{ "siphash", test_siphash }, { "proto", test_proto },   { "replay", test_replay },
		{ "arrival", test_arrival }, { "config", test_config }, { "util", test_util },
		{ "idmap", test_idmap },   { "health", test_health }, { "blake2s", test_blake2s },
		{ "pair", test_pair },       { "ctl", test_ctl },
	};

	for (unsigned i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
		int before = cg_test_failures;

		suites[i].fn();
		printf("%-8s %s\n", suites[i].name, cg_test_failures == before ? "ok" : "FAILED");
	}
	printf("%d checks, %d failures\n", cg_test_checks, cg_test_failures);
	return cg_test_failures ? 1 : 0;
}
