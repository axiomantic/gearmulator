/* alloc() turns a cycles-for-each-frame rational into a whole-cycle allocation
 * for each quantum. It is exact, it uses no floating point and it reads no wall
 * clock.
 *
 * The expected sequence is computed from G2_DSP_CYCLES_PER_FRAME_NUM and
 * G2_DSP_CYCLES_PER_FRAME_DEN, and no figure is written down here. The
 * numerator is still provisional, and with the sequence written down a new
 * numerator would fail this test with no message that connects the failure to
 * the change.
 *
 * A negative case pins the numerator to a scratch value and asserts the derived
 * sequence moves with it, so the derivation is known to be live rather than
 * decorative. Without that case a derivation that silently returned the same
 * answer for every numerator would pass.
 */

#include "g2/timebase.h"

#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <vector>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqual(const uint64_t observed, const uint64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what,
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	uint32_t greatestCommonDivisor(uint32_t a, uint32_t b) noexcept
	{
		while(b != 0u)
		{
			const uint32_t t = a % b;
			a = b;
			b = t;
		}
		return a;
	}

	/* The period of the accumulator, in frames. After this many calls from a
	 * zero accumulator the accumulator is zero again, so a sum taken over a
	 * whole number of periods carries no part-period remainder and the mean
	 * is exact in integer arithmetic. */
	uint32_t accumulatorPeriod(const Rational r) noexcept
	{
		const uint32_t rem = r.num % r.den;
		if(rem == 0u)
			return 1u;
		return r.den / greatestCommonDivisor(rem, r.den);
	}

	/* The DERIVATION, stated once and used by every case below. It repeats
	 * none of alloc()'s code: it is the closed form of the same rule.
	 *
	 *   whole(i) = num/den, plus one when the accumulator crosses den
	 *   acc(i)   = ((i + 1) * (num % den)) mod den
	 *
	 * All arithmetic is 64-bit integer. There is no floating point anywhere
	 * in this file. */
	struct Derived
	{
		uint32_t whole;
		uint32_t acc;
	};

	Derived derive(const Rational r, const uint32_t frameIndex) noexcept
	{
		const uint32_t low = r.num / r.den;
		const uint32_t rem = r.num % r.den;

		const uint64_t accBefore =
			(static_cast<uint64_t>(frameIndex) * rem) % r.den;
		const uint64_t accRaw = accBefore + rem;

		Derived d{};
		d.whole = low + (accRaw >= r.den ? 1u : 0u);
		d.acc   = static_cast<uint32_t>(accRaw % r.den);
		return d;
	}

	/* Drives alloc() over `frames` quanta from a zero accumulator and returns
	 * the observed sequence. Every invariant that does not need the
	 * derivation is asserted inside the loop. */
	std::vector<uint32_t> driveAlloc(const Rational r, const uint32_t frames,
		const char* const what)
	{
		std::vector<uint32_t> observed;
		observed.reserve(frames);

		uint32_t acc = 0u;

		for(uint32_t i = 0; i < frames; ++i)
		{
			/* The accumulator stays below the denominator at entry. */
			if(acc >= r.den)
			{
				printf("FAIL %s: accumulator %u is not below the denominator "
					"%u at entry to frame %u\n", what, acc, r.den, i);
				++failures;
				return observed;
			}

			const uint32_t whole = alloc(r, &acc);

			/* And at exit. */
			if(acc >= r.den)
			{
				printf("FAIL %s: accumulator %u is not below the denominator "
					"%u at exit from frame %u\n", what, acc, r.den, i);
				++failures;
				return observed;
			}

			observed.push_back(whole);
		}

		return observed;
	}

	/* Holds alloc() against the derivation, frame by frame, and then against
	 * the exact mean. Returns the sequence so a caller can compare two
	 * rationals against each other. */
	std::vector<uint32_t> assertSequence(const Rational r, const uint32_t frames,
		const char* const what)
	{
		const std::vector<uint32_t> observed = driveAlloc(r, frames, what);

		if(observed.size() != frames)
		{
			printf("FAIL %s: the drive stopped after %zu of %u frames\n", what,
				observed.size(), frames);
			++failures;
			return observed;
		}

		const uint32_t low  = r.num / r.den;
		const uint32_t rem  = r.num % r.den;
		const uint32_t high = low + (rem != 0u ? 1u : 0u);

		uint32_t replayAcc  = 0u;
		uint64_t sum        = 0u;
		bool     lowSeen    = false;
		bool     highSeen   = false;

		for(uint32_t i = 0; i < frames; ++i)
		{
			const Derived expected = derive(r, i);

			if(observed[i] != expected.whole)
			{
				printf("FAIL %s: frame %u returned %u, the derivation gives "
					"%u\n", what, i, observed[i], expected.whole);
				++failures;
				return observed;
			}

			/* The accumulator state is an observable of its own, so it is
			 * held against the derivation and not merely bounded. A body
			 * that returned the right allocation while leaving the wrong
			 * remainder would pass a value-only check and then drift. */
			(void) alloc(r, &replayAcc);
			if(replayAcc != expected.acc)
			{
				printf("FAIL %s: the accumulator after frame %u is %u, the "
					"derivation gives %u\n", what, i, replayAcc, expected.acc);
				++failures;
				return observed;
			}

			/* Exactly two adjacent values, and no third. */
			if(observed[i] != low && observed[i] != high)
			{
				printf("FAIL %s: frame %u returned %u, which is neither %u "
					"nor %u\n", what, i, observed[i], low, high);
				++failures;
				return observed;
			}

			lowSeen  = lowSeen  || observed[i] == low;
			highSeen = highSeen || observed[i] == high;

			sum += observed[i];
		}

		/* The exact mean, in integers. `frames` is a whole number of
		 * accumulator periods, so sum/frames is exactly num/den and the
		 * cross-multiplied form below carries no rounding at all. */
		checkEqual(sum * r.den, static_cast<uint64_t>(frames) * r.num,
			what);

		/* Both adjacent values really occur whenever the ratio is not a whole
		 * number. Without this a body that always returned `low` would pass
		 * every bound above for a rational whose remainder it ignored. */
		if(rem != 0u)
		{
			check(lowSeen, "the lower adjacent value occurs");
			check(highSeen, "the higher adjacent value occurs");
		}

		return observed;
	}

	uint32_t framesForAtLeast(const Rational r, const uint32_t atLeast) noexcept
	{
		const uint32_t period = accumulatorPeriod(r);
		return period * ((atLeast + period - 1u) / period);
	}
}

/* The arithmetic is integer throughout, and that is asserted rather than
 * described. A floating-point step anywhere in the rational would make the
 * allocation inexact at some numerator, and the whole point of the rational is
 * that it is exact at every numerator. */
static_assert(std::is_integral_v<decltype(Rational::num)>,
	"Rational::num is an integer.");
static_assert(std::is_integral_v<decltype(Rational::den)>,
	"Rational::den is an integer.");
static_assert(std::is_same_v<decltype(alloc(Rational{1u, 1u},
	static_cast<uint32_t*>(nullptr))), uint32_t>,
	"alloc returns a whole cycle count, so its type is an unsigned integer.");

int main()
{
	/* ---------------- the shipped rational.
	 *
	 * Both figures come from g2/timebase.h and neither is written here. */
	const Rational shipped =
	{
		G2_DSP_CYCLES_PER_FRAME_NUM,
		G2_DSP_CYCLES_PER_FRAME_DEN
	};

	check(shipped.den != 0u, "the shipped denominator is not zero");

	const uint32_t shippedFrames = framesForAtLeast(shipped, 1000u);

	const std::vector<uint32_t> shippedSequence =
		assertSequence(shipped, shippedFrames, "the shipped DSP rational");

	/* ---------------- the negative case: a scratch numerator.
	 *
	 * 100,000,000 is a SCRATCH FIXTURE VALUE and it names nothing about the
	 * machine. It is here to prove the derivation above is live: if the
	 * expected sequence were written down rather than computed, the two
	 * sequences below would be identical and this case would fail.
	 *
	 * The scratch numerator is chosen so that its remainder against the frame
	 * rate differs from the shipped one, which is what makes the sequences
	 * differ at all. That property is asserted rather than assumed. */
	const uint32_t kScratchNumerator = 100000000u;

	const Rational scratch = { kScratchNumerator, G2_DSP_CYCLES_PER_FRAME_DEN };

	check(scratch.num % scratch.den != shipped.num % shipped.den,
		"the scratch numerator has a different remainder, so the sequences "
		"must differ");

	const uint32_t scratchFrames = framesForAtLeast(scratch, 1000u);

	const std::vector<uint32_t> scratchSequence =
		assertSequence(scratch, scratchFrames, "the scratch rational");

	/* The derived sequence moves with the numerator. */
	{
		const uint32_t compared =
			shippedFrames < scratchFrames ? shippedFrames : scratchFrames;

		bool differs = false;
		for(uint32_t i = 0; i < compared && !differs; ++i)
			differs = shippedSequence[i] != scratchSequence[i];

		check(differs, "the sequence a scratch numerator produces differs "
			"from the shipped one");
	}

	/* ---------------- the accumulator is honoured on entry, not assumed zero.
	 *
	 * A context is snapshotted and restored, so alloc() is entered with a
	 * non-zero accumulator in normal operation. Driving one frame from every
	 * legal starting value of a small rational holds the whole rule -- the
	 * carry, the subtraction and the bound -- against every branch it has. */
	{
		const Rational small = { 7u, 4u };   /* 1.75 cycles for each frame */

		for(uint32_t start = 0; start < small.den; ++start)
		{
			uint32_t acc = start;
			const uint32_t whole = alloc(small, &acc);

			const uint32_t low = small.num / small.den;   /* 1 */
			const uint32_t rem = small.num % small.den;   /* 3 */

			const uint32_t expectedWhole = low + (start + rem >= small.den ? 1u : 0u);
			const uint32_t expectedAcc   = (start + rem) % small.den;

			checkEqual(whole, expectedWhole,
				"the allocation for a non-zero starting accumulator");
			checkEqual(acc, expectedAcc,
				"the accumulator after a non-zero starting accumulator");
			check(acc < small.den,
				"the accumulator stays below the denominator");
		}
	}

	/* ---------------- a whole ratio carries no remainder at all. */
	{
		const Rational whole = { 96000u * 13u, 96000u };

		uint32_t acc = 0u;
		for(uint32_t i = 0; i < 64u; ++i)
		{
			checkEqual(alloc(whole, &acc), 13u,
				"a whole ratio allocates the same count every frame");
			checkEqual(acc, 0u,
				"a whole ratio leaves the accumulator at zero");
		}
	}

	/* ---------------- the allocation is a pure function of the state.
	 *
	 * Two drives from the same starting accumulator give the same sequence.
	 * A body that read a wall clock, a frame counter of its own or any other
	 * hidden state would not. This case is the behavioural half of the rule
	 * that no scheduler file reads a system clock. */
	{
		const std::vector<uint32_t> first =
			driveAlloc(shipped, 512u, "the first pure-function drive");
		const std::vector<uint32_t> second =
			driveAlloc(shipped, 512u, "the second pure-function drive");

		check(first == second,
			"two drives from the same state give the same sequence");
	}

	if(failures != 0)
	{
		printf("t0_alloc: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_alloc: all cases passed\n");
	return 0;
}
