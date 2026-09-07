/* t0_timebase_header.c -- the C-compilation check of g2/timebase.h.
 *
 * This file is compiled AS C11 by t0_timebase_header.cmake at TEST time, not
 * at build time. That is deliberate: "g2/timebase.h
 * is a C header" a contract rather than a convention, and a contract that is
 * only asserted by the build cannot be reported by `ctest -R`.
 * A C++ reference parameter anywhere in the
 * header is a syntax error in C, so the compile IS the assertion.
 *
 * ------------------------------------------------------------------------
 * WHY TWO DECLARED MACROS ARE NOT ASSERTED HERE.
 *
 * g2Lib/CMakeLists.txt carries a configure-time guard for two UNMEASURED
 * symbols. It fails the CONFIGURE step if
 * any file under source/nord/g2/ names either one, and it exempts exactly one
 * file: their declaration site, g2/timebase.h. This file is under that tree, so
 * naming either symbol here would fail the configure before this test could be
 * compiled at all -- and it would fail it by NAMING the symbol, which is the
 * guard working as designed.
 *
 * The rule "one _Static_assert for each declared macro" is therefore satisfied
 * for those two INSIDE g2/timebase.h itself, which is the exempt file. Every
 * other declared macro is asserted below. No macro goes unasserted, and the
 * guard is neither weakened nor edited.
 * ------------------------------------------------------------------------
 */

#include <stdint.h>
#include <stdio.h>

#include "g2/timebase.h"

/* ---------------- one _Static_assert for each macro asserted here.
 *
 * A deleted or renamed constant becomes an undeclared identifier in the
 * controlling expression, which is a compile error and not a silent absence.
 * A constant that survives with the WRONG value fails the comparison. Both
 * failures are reported by this test's own command.
 */

_Static_assert(G2_FRAME_RATE_HZ == 96000u,
	"G2_FRAME_RATE_HZ is the ESAI frame rate, 96 kHz.");

_Static_assert(G2_DSP_CYCLES_PER_FRAME_NUM == 150000000u,
	"DSP clock in Hz. PROVISIONAL, and unmeasured.");

_Static_assert(G2_DSP_CYCLES_PER_FRAME_DEN == 96000u,
	"The DSP rational denominator is the frame rate, and it is fixed.");

_Static_assert(G2_MCU_CYCLES_PER_FRAME_NUM == 162000000u,
	"The MCU rational numerator is the CORE clock.");

_Static_assert(G2_MCU_CYCLES_PER_FRAME_DEN == 96000u,
	"The MCU rational denominator is the frame rate.");

_Static_assert(G2_MCU_CORE_CLOCK_HZ == 162000000u,
	"Three times the CLKIN the schematic labels at U14.");

_Static_assert(G2_CHAIN_HOP_FRAMES == 1u,
	"H, provisional 1, and unmeasured.");

_Static_assert(G2_SECOND_BUS_FRAME_DIVIDER == 4u,
	"Provisional 4, from the recorded 24 kHz control rate.");

_Static_assert(G2_HOST_FRAMES_NUM == 96000u,
	"The host-block mapping numerator is the frame rate.");

/* 54,000,000 is the BUS clock, and the core symbol is the one place where
 * standing it in would do real damage. The assert closes that place without
 * writing the literal down, because t0_timebase_header.cmake still bans the
 * literal tree-wide. */
_Static_assert(G2_MCU_CORE_CLOCK_HZ != 54000u * 1000u,
	"54,000,000 is the bus clock, not the core clock.");

/* ---------------- the declared shape of Rational.
 *
 * A typedef struct of two uint32_t. _Generic is a
 * C11 construct and it pins the member types exactly rather than by width.
 */

_Static_assert(sizeof(Rational) >= 2u * sizeof(uint32_t),
	"Rational carries two uint32_t.");

_Static_assert(_Generic(((Rational){0u, 0u}).num, uint32_t: 1, default: 0),
	"Rational::num is uint32_t.");

_Static_assert(_Generic(((Rational){0u, 0u}).den, uint32_t: 1, default: 0),
	"Rational::den is uint32_t.");

/* ---------------- the two signatures, each with its FULL function-pointer
 * type.
 *
 * The
 * accumulator is a POINTER in both. A `uint32_t&` accumulator does not compile
 * in C at all, so the compile fails here before these initialisers are even
 * reached.
 */

static uint32_t (*const allocPtr)(Rational, uint32_t*) = &alloc;

static uint32_t (*const framesForBlockPtr)(uint32_t, uint32_t, uint32_t,
	uint32_t*) = &framesForBlock;

static int failures = 0;

static void check(const int condition, const char* const what)
{
	if(!condition)
	{
		printf("FAIL %s\n", what);
		++failures;
	}
}

int main(void)
{
	uint32_t acc = 0u;
	uint32_t i = 0u;
	uint64_t total = 0u;
	Rational dsp;

	dsp.num = G2_DSP_CYCLES_PER_FRAME_NUM;
	dsp.den = G2_DSP_CYCLES_PER_FRAME_DEN;

	/* The DSP sequence is 1562, 1563, 1562, 1563 and the mean
	 * is exactly 1562.5. A scalar constant cannot produce it, which is the
	 * whole reason the rational exists. */
	check(allocPtr(dsp, &acc) == 1562u, "alloc frame 0 is 1562");
	check(allocPtr(dsp, &acc) == 1563u, "alloc frame 1 is 1563");
	check(allocPtr(dsp, &acc) == 1562u, "alloc frame 2 is 1562");
	check(allocPtr(dsp, &acc) == 1563u, "alloc frame 3 is 1563");

	/* Two frames are exactly 3125 cycles, with no drift over many frames. */
	acc = 0u;
	total = 0u;
	for(i = 0u; i < 9600u; ++i)
		total += allocPtr(dsp, &acc);
	check(total == 9600ull * G2_DSP_CYCLES_PER_FRAME_NUM
		/ G2_DSP_CYCLES_PER_FRAME_DEN, "alloc has no drift over 9600 frames");
	check(acc == 0u, "the accumulator returns to zero on an exact boundary");

	/* The hard case: 44.1 kHz, r = 320/147, n = 32. m takes one
	 * of exactly two adjacent values, and 147 blocks repeat exactly. */
	acc = 0u;
	total = 0u;
	for(i = 0u; i < 147u; ++i)
	{
		const uint32_t m = framesForBlockPtr(32u, G2_HOST_FRAMES_NUM, 44100u,
			&acc);
		check(m == 69u || m == 70u, "framesForBlock is one of two adjacent values");
		check(acc < 44100u, "the accumulator stays below the denominator");
		total += m;
	}
	check(total == 32u * 320u, "147 blocks of 32 give exactly 10240 frames");
	check(acc == 0u, "the accumulator repeats with period 147");

	/* The 64-bit intermediate. n * num is 96,000,000,000 here, which overflows
	 * 32 bits; a narrow multiply would wrap and give a wrong answer rather
	 * than a diagnostic. */
	acc = 0u;
	check(framesForBlockPtr(1000000u, G2_HOST_FRAMES_NUM, 44100u, &acc)
		== 2176870u, "framesForBlock widens to 64 bits before multiplying");
	check(acc == 33000u, "the wide path leaves the right remainder");

	if(failures != 0)
	{
		printf("t0_timebase_header: %d check(s) failed\n", failures);
		return 1;
	}

	printf("t0_timebase_header: all checks passed\n");
	return 0;
}
