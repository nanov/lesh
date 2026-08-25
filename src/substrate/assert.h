#pragma once

#include <cstdio>
#include <cstdlib>

// Invariant violations are programmer errors, not user errors: die loudly and
// immediately rather than continuing in a state we have already established is
// impossible. See the error-handling section of the scope spec.
//
// Live in Debug and RelWithDebInfo, compiled out in Release. Keeping it in the
// benchmark configuration means a profiling run still catches a broken
// invariant instead of quietly producing a fast wrong answer.
#ifdef LESH_ENABLE_ASSERTS
#define LESH_ASSERT(cond)                                                      \
	do {                                                                       \
		if (!(cond)) [[unlikely]] {                                            \
			std::fprintf(stderr,                                               \
			             "lesh: assertion failed: %s\n"                        \
			             "  at %s:%d in %s\n",                                 \
			             #cond, __FILE__, __LINE__, __func__);                 \
			std::abort();                                                      \
		}                                                                      \
	} while (0)
#else
#define LESH_ASSERT(cond) ((void)0)
#endif
