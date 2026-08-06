/* t0_timebase_header.c -- the check of task SCH-0.
 *
 * This file is compiled AS C11 by t0_timebase_header.cmake at TEST time, not
 * at build time. That is deliberate: design section 13.4.1 makes "g2/timebase.h
 * is a C header" a contract rather than a convention, and a contract that is
 * only asserted by the build cannot be reported by `ctest -R`. Plan section
 * 7.7.1 draws the same distinction. A C++ reference parameter anywhere in the
 * header -- the `uint32_t& acc` an earlier draft carried -- is a syntax error
 * in C, so the compile IS the assertion.
 *
 * ------------------------------------------------------------------------
 * WHY TWO DECLARED MACROS ARE NOT ASSERTED HERE.
 *
 * g2Lib/CMakeLists.txt carries a configure-time guard for the two UNMEASURED
 * symbols of measurement register rows 5 and 6. It fails the CONFIGURE step if
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
	"G2_FRAME_RATE_HZ is the ESAI frame rate, 96 kHz. Design 13.4.1.");

_Static_assert(G2_DSP_CYCLES_PER_FRAME_NUM == 150000000u,
	"DSP clock in Hz. PROVISIONAL, register row 8, owner SPK-8 criterion (e).");

_Static_assert(G2_DSP_CYCLES_PER_FRAME_DEN == 96000u,
	"The DSP rational denominator is the frame rate, and it is fixed.");

_Static_assert(G2_MCU_CYCLES_PER_FRAME_NUM == 45000000u,
	"The MCU rational numerator is the CORE clock. Design 13.4.1, 13.4.3.");

_Static_assert(G2_MCU_CYCLES_PER_FRAME_DEN == 96000u,
	"The MCU rational denominator is the frame rate.");

_Static_assert(G2_MCU_CORE_CLOCK_HZ == 45000000u,
	"PLACEHOLDER, register row 7: the lowest in-spec catalog speed grade.");

_Static_assert(G2_CHAIN_HOP_FRAMES == 1u,
	"H, provisional 1. Register row 9, owner SPK-3 criterion (d).");

_Static_assert(G2_SECOND_BUS_FRAME_DIVIDER == 4u,
	"Provisional 4, from the recorded 24 kHz control rate. Plan section 4.4.");

_Static_assert(G2_HOST_FRAMES_NUM == 96000u,
	"The host-block mapping numerator is the frame rate. Design 14.1.1.");

/* The refuted MCU clock must never come back. Register row 7 and design
 * section 13.4.3 record it as REFUTED rather than unverified. The grep case in
 * t0_timebase_header.cmake asserts the literal appears nowhere in this
 * repository; this assert closes the one place the literal would do real
 * damage, and it does so without writing the literal down. */
_Static_assert(G2_MCU_CORE_CLOCK_HZ != 54000u * 1000u,
	"54,000,000 is REFUTED by five independent objections. Design 13.4.3.");

/* ---------------- the declared shape of Rational.
 *
 * A typedef struct of two uint32_t, per design section 13.4.1. _Generic is a
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
 * This is the C analogue of the address-of expressions SCH-4 uses. The
 * accumulator is a POINTER in both. An earlier draft wrote `uint32_t& acc`,
 * which does not compile in C at all -- and if it ever returns, the compile
 * fails here before these initialisers are even reached.
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

	/* Design 13.4.1: the DSP sequence is 1562, 1563, 1562, 1563 and the mean
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

	/* Design 14.1.1, the hard case: 44.1 kHz, r = 320/147, n = 32. m takes one
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
	 * than a diagnostic. Design 14.1.1 requires the widening. */
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
