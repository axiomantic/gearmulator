/* t0_frames_for_block.cpp -- the check of task SCH-2. Design section 14.1.1.
 *
 * framesForBlock() maps a host block of n samples to whole 96 kHz frames. It
 * is the same accumulator shape as alloc(), WIDENED TO 64 BITS because n * num
 * overflows 32 bits at a large block size.
 *
 * The range of m is exact. Write r = num / den. For every block,
 *
 *     floor(n * r)  <=  m  <=  floor(n * r) + 1
 *
 * because the accumulator is below den at entry and at exit, so m takes one of
 * exactly TWO adjacent integer values and the long-run mean is exactly n * r
 * with no drift.
 *
 * THIS ACCUMULATOR SITS OUTSIDE THE DETERMINISM BOUNDARY. It lives in the
 * framework's ResamplerInOut, one layer above the Device. This test checks the
 * SHAPE the design specifies for the accumulator this project owns. It is not
 * a requirement the adopted framework component fails.
 *
 * The numerator comes from g2/timebase.h. The host rates are FIXTURE VALUES:
 * a host rate is not a property of the machine and no shipped header carries
 * one.
 */

#include "g2/timebase.h"

#include <cstdint>
#include <cstdio>
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

	/* floor(n * num / den), in 64-bit integers. This is the closed form of
	 * the same rule framesForBlock carries, and it is NOT a copy of its body:
	 * it takes no accumulator and it never carries. */
	uint64_t floorFrames(const uint64_t n, const uint32_t num,
		const uint32_t den) noexcept
	{
		return (n * num) / den;
	}

	/* Drives a whole sequence of blocks through one accumulator and holds the
	 * CUMULATIVE state against the closed form after every block.
	 *
	 * The cumulative statement is what "no fraction is lost" means: after
	 * blocks of n0, n1 ... nk the total is floor((n0 + n1 + ... + nk) * r) and
	 * the accumulator is that product's remainder. A body that dropped the
	 * remainder at a block boundary, or that reset the accumulator when n
	 * changed, would still pass a per-block bound and would fail here. */
	uint64_t driveBlocks(const std::vector<uint32_t>& blocks,
		const uint32_t num, const uint32_t den, const char* const what)
	{
		uint32_t acc            = 0u;
		uint64_t cumulativeN    = 0u;
		uint64_t cumulativeM    = 0u;

		for(size_t b = 0; b < blocks.size(); ++b)
		{
			const uint32_t n = blocks[b];

			if(acc >= den)
			{
				printf("FAIL %s: accumulator %u is not below the denominator "
					"%u at entry to block %zu\n", what, acc, den, b);
				++failures;
				return cumulativeM;
			}

			const uint32_t m = framesForBlock(n, num, den, &acc);

			if(acc >= den)
			{
				printf("FAIL %s: accumulator %u is not below the denominator "
					"%u at exit from block %zu\n", what, acc, den, b);
				++failures;
				return cumulativeM;
			}

			/* EXACTLY TWO ADJACENT VALUES, and no third. */
			const uint64_t low = floorFrames(n, num, den);

			if(m < low || m > low + 1u)
			{
				printf("FAIL %s: block %zu of %u samples returned %u, which is "
					"outside [%llu, %llu]\n", what, b, n, m,
					static_cast<unsigned long long>(low),
					static_cast<unsigned long long>(low + 1u));
				++failures;
				return cumulativeM;
			}

			cumulativeN += n;
			cumulativeM += m;

			/* The cumulative identity, after every block. */
			const uint64_t expectedCumulativeM =
				floorFrames(cumulativeN, num, den);
			const uint32_t expectedAcc = static_cast<uint32_t>(
				(cumulativeN * num) % den);

			if(cumulativeM != expectedCumulativeM)
			{
				printf("FAIL %s: after block %zu the total is %llu, the closed "
					"form gives %llu\n", what, b,
					static_cast<unsigned long long>(cumulativeM),
					static_cast<unsigned long long>(expectedCumulativeM));
				++failures;
				return cumulativeM;
			}

			if(acc != expectedAcc)
			{
				printf("FAIL %s: after block %zu the accumulator is %u, the "
					"closed form gives %u\n", what, b, acc, expectedAcc);
				++failures;
				return cumulativeM;
			}
		}

		return cumulativeM;
	}
}

int main()
{
	/* The numerator is the ESAI frame rate and it comes from the header. */
	const uint32_t num = G2_HOST_FRAMES_NUM;

	checkEqual(num, G2_FRAME_RATE_HZ,
		"the host-block numerator is the frame rate");

	/* ---------------- the hard case: 44.1 kHz.
	 *
	 * r = 96000/44100 = 320/147 in lowest terms. At n = 32, n * r is 69.65...
	 * so m is 69 or 70, and the pattern repeats every 147 blocks. Every one of
	 * those statements is DERIVED below and none is written down as a result.
	 */
	{
		const uint32_t hostRate = 44100u;
		const uint32_t n        = 32u;

		/* r in lowest terms. 320/147 is what this must come to, and the
		 * assertion says so from the two rates rather than from the pair. */
		const uint32_t g = greatestCommonDivisor(num, hostRate);
		checkEqual(num / g, 320u, "r's numerator in lowest terms");
		checkEqual(hostRate / g, 147u, "r's denominator in lowest terms");

		/* m is 69 or 70, derived. */
		const uint64_t low = floorFrames(n, num, hostRate);
		checkEqual(low, 69u, "floor(32 * 320/147)");

		/* The period, derived from the remainder. */
		const uint32_t blockRemainder =
			static_cast<uint32_t>((static_cast<uint64_t>(n) * num) % hostRate);
		const uint32_t period =
			hostRate / greatestCommonDivisor(blockRemainder, hostRate);
		checkEqual(period, 147u, "the accumulator period at 44.1 kHz, n = 32");

		const std::vector<uint32_t> blocks(period, n);

		const uint64_t total =
			driveBlocks(blocks, num, hostRate, "44.1 kHz, blocks of 32");

		/* THE MEAN OVER 147 BLOCKS IS EXACTLY 32 * 320/147.
		 *
		 * Asserted by cross-multiplication so that no division and no rounding
		 * enters: total / period == n * num / hostRate becomes
		 * total * hostRate == period * n * num. */
		checkEqual(total * hostRate,
			static_cast<uint64_t>(period) * n * num,
			"the mean over one whole period is exactly n * r");

		/* And the same statement in its plainest form: a whole period of 32
		 * sample blocks at 44.1 kHz is 10,240 frames, because 147 blocks of 32
		 * samples is 4,704 samples and 4,704 * 320/147 is 10,240 exactly. */
		checkEqual(total, 10240u, "the frame total over one whole period");

		/* Both adjacent values really occur. */
		{
			uint32_t acc      = 0u;
			bool     lowSeen  = false;
			bool     highSeen = false;

			for(uint32_t b = 0; b < period; ++b)
			{
				const uint32_t m = framesForBlock(n, num, hostRate, &acc);
				lowSeen  = lowSeen  || m == static_cast<uint32_t>(low);
				highSeen = highSeen || m == static_cast<uint32_t>(low + 1u);
			}

			check(lowSeen, "the lower of the two adjacent values occurs");
			check(highSeen, "the higher of the two adjacent values occurs");
		}
	}

	/* ---------------- a whole ratio carries no remainder.
	 *
	 * At 48 kHz r is exactly 2, so m never moves and the accumulator never
	 * leaves zero. A body that always carried would fail here. */
	{
		const uint32_t hostRate = 48000u;

		uint32_t acc = 0u;
		for(uint32_t b = 0; b < 256u; ++b)
		{
			checkEqual(framesForBlock(128u, num, hostRate, &acc), 256u,
				"a whole ratio maps every block the same way");
			checkEqual(acc, 0u, "a whole ratio leaves the accumulator at zero");
		}
	}

	/* ---------------- a mid-stream block-size change loses no fraction.
	 *
	 * n enters the formula for EACH block and the accumulator carries the
	 * remainder across the change unaltered. The proof is that a run of
	 * changing block sizes reaches the same frame total as ONE block of the
	 * same total sample count, from the same starting accumulator. */
	{
		const uint32_t hostRate = 44100u;

		const std::vector<uint32_t> changing =
		{
			32u, 32u, 17u, 17u, 256u, 1u, 1u, 1u, 480u, 64u, 63u, 512u, 33u
		};

		uint64_t totalSamples = 0u;
		for(const uint32_t n : changing)
			totalSamples += n;

		const uint64_t viaBlocks =
			driveBlocks(changing, num, hostRate, "a changing block size");

		/* The same sample count delivered as one block. */
		uint32_t oneAcc = 0u;
		const uint32_t viaOneBlock = framesForBlock(
			static_cast<uint32_t>(totalSamples), num, hostRate, &oneAcc);

		checkEqual(viaBlocks, viaOneBlock,
			"a changing block size reaches the same total as one block");

		/* And the total is the closed form, so neither route is merely
		 * consistent with the other while both are wrong. */
		checkEqual(viaBlocks, floorFrames(totalSamples, num, hostRate),
			"the total after a block-size change is floor(N * r)");
	}

	/* ---------------- the intermediate arithmetic is 64-bit.
	 *
	 * n * num overflows 32 bits above n = 44,739 at this numerator, and the
	 * bound is DERIVED here rather than written down. A 32-bit intermediate
	 * wraps and returns a small number; the closed form below is computed in
	 * 64 bits and the two disagree at once. */
	{
		const uint32_t hostRate = 44100u;

		const uint32_t overflowAtLeast =
			static_cast<uint32_t>(0xFFFFFFFFull / num) + 1u;

		check(overflowAtLeast > 1u,
			"the block size at which a 32-bit product would overflow is "
			"greater than one");

		const uint32_t n = overflowAtLeast * 2u;       /* comfortably past it */

		check(static_cast<uint64_t>(n) * num > 0xFFFFFFFFull,
			"the chosen block size really does overflow a 32-bit product");

		uint32_t acc = 0u;
		const uint32_t m = framesForBlock(n, num, hostRate, &acc);

		checkEqual(m, floorFrames(n, num, hostRate),
			"a block large enough to overflow 32 bits still maps exactly");
		checkEqual(acc,
			static_cast<uint32_t>((static_cast<uint64_t>(n) * num) % hostRate),
			"the accumulator after a 64-bit sized block");
	}

	/* ---------------- a long run at 44.1 kHz shows no drift.
	 *
	 * 1,000 whole periods. The cumulative identity inside driveBlocks holds
	 * after every one of the 147,000 blocks, so a drift of one frame anywhere
	 * in the run is caught at the block that causes it and not only at the
	 * end. */
	{
		const uint32_t hostRate = 44100u;
		const uint32_t n        = 32u;
		const uint32_t period   = 147u;

		const std::vector<uint32_t> blocks(period * 1000u, n);

		const uint64_t total =
			driveBlocks(blocks, num, hostRate, "1,000 periods at 44.1 kHz");

		checkEqual(total * hostRate,
			static_cast<uint64_t>(period) * 1000u * n * num,
			"1,000 whole periods carry no drift at all");
	}

	if(failures != 0)
	{
		printf("t0_frames_for_block: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_frames_for_block: all cases passed\n");
	return 0;
}
