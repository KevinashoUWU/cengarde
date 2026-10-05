/* A small classic BPF checker and interpreter for the tests, enough for the
 * steering programs of steer.h: LD LEN, LD B ABS, LD IMM, AND, ADD, TAX,
 * JEQ, JGE, JA and RET. It runs a program the way the kernel runs a
 * SO_REUSEPORT program, on the UDP payload: a load past the end ends it
 * with 0.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_TEST_CBPF_H
#define CG_TEST_CBPF_H

#include <linux/filter.h>
#include <stddef.h>
#include <stdint.h>

/* Whether prog (n instructions) is one this interpreter, and the kernel,
 * accept: at most BPF_MAXINSNS instructions, only the opcodes above, every
 * jump forward and inside the program (a conditional one in its 8 bits),
 * and a RET last, so that it always ends. NULL, or why not. */
static inline const char *cbpf_check(const struct sock_filter *prog, int n)
{
	if (n <= 0 || n > BPF_MAXINSNS)
		return "length";
	for (int pc = 0; pc < n; pc++) {
		const struct sock_filter *f = &prog[pc];

		switch (f->code) {
		case BPF_LD | BPF_W | BPF_LEN:
		case BPF_LD | BPF_B | BPF_ABS:
		case BPF_LD | BPF_IMM:
		case BPF_ALU | BPF_AND | BPF_K:
		case BPF_ALU | BPF_ADD | BPF_K:
		case BPF_ALU | BPF_ADD | BPF_X:
		case BPF_MISC | BPF_TAX:
		case BPF_RET | BPF_A:
		case BPF_RET | BPF_K:
			break;
		case BPF_JMP | BPF_JA:
			if (f->k >= (uint32_t)(n - pc - 1))
				return "ja out of the program";
			break;
		case BPF_JMP | BPF_JEQ | BPF_K:
		case BPF_JMP | BPF_JGE | BPF_K:
			/* jt and jf are 8 bits: a generator that overflowed them
			 * shows up as a target in the wrong place, which the
			 * tests catch by running; here, only the range. */
			if (f->jt >= n - pc - 1 || f->jf >= n - pc - 1)
				return "conditional jump out of the program";
			break;
		default:
			return "unknown opcode";
		}
	}
	if (BPF_CLASS(prog[n - 1].code) != BPF_RET)
		return "not ending in RET";
	return NULL;
}

/* Runs a program that passed cbpf_check on the len bytes at pkt. */
static inline uint32_t cbpf_run(const struct sock_filter *prog, int n, const uint8_t *pkt, uint32_t len)
{
	uint32_t a = 0, x = 0;

	for (int pc = 0; pc < n; pc++) {
		const struct sock_filter *f = &prog[pc];

		switch (f->code) {
		case BPF_LD | BPF_W | BPF_LEN:
			a = len;
			break;
		case BPF_LD | BPF_B | BPF_ABS:
			if (f->k >= len)
				return 0;
			a = pkt[f->k];
			break;
		case BPF_LD | BPF_IMM:
			a = f->k;
			break;
		case BPF_ALU | BPF_AND | BPF_K:
			a &= f->k;
			break;
		case BPF_ALU | BPF_ADD | BPF_K:
			a += f->k;
			break;
		case BPF_ALU | BPF_ADD | BPF_X:
			a += x;
			break;
		case BPF_MISC | BPF_TAX:
			x = a;
			break;
		case BPF_JMP | BPF_JA:
			pc += (int)f->k;
			break;
		case BPF_JMP | BPF_JEQ | BPF_K:
			pc += a == f->k ? f->jt : f->jf;
			break;
		case BPF_JMP | BPF_JGE | BPF_K:
			pc += a >= f->k ? f->jt : f->jf;
			break;
		case BPF_RET | BPF_A:
			return a;
		case BPF_RET | BPF_K:
			return f->k;
		default:
			return 0;
		}
	}
	return 0;
}

#endif
