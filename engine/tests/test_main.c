/* SPDX-License-Identifier: GPL-2.0-only */
#include "test.h"

int cg_test_failures;
int cg_test_checks;

int main(void)
{
	/* The Makefile builds every tests/test_*.c; a new suite adds its line to
	 * suites.h. */
	static const struct {
		const char *name;
		void (*fn)(void);
	} suites[] = {
#define SUITE(name) { #name, test_##name },
#include "suites.h"
#undef SUITE
	};

	for (unsigned i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
		int before = cg_test_failures;

		suites[i].fn();
		printf("%-8s %s\n", suites[i].name, cg_test_failures == before ? "ok" : "FAILED");
	}
	printf("%d checks, %d failures\n", cg_test_checks, cg_test_failures);
	return cg_test_failures ? 1 : 0;
}
