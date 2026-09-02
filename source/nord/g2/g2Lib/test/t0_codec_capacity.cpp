/* The capacity arithmetic, not the surface. T0_codec_queue_surface holds the
 * declared members and the refusal. This row drives blocks through push,
 * runFrames and pull in that order and asserts that a capacity of L + B is
 * enough and that nothing is dropped, overflowed or short-supplied.
 *
 * The "runFrames" here is not the Scheduler's. The fixture below supplies the
 * part of a quantum that touches the two codec queues and nothing else: the
 * ingress reads front() and pops one frame from the CodecSource, and the egress
 * pushes one frame into the CodecSink. That is the whole of what the capacity
 * rule depends on -- the call order of push, runFrames and pull, and one frame
 * in and one frame out for each of the block's frames.
 *
 * The sink is primed with L frames, and without that this check cannot fail for
 * the part it exists to test. The lookahead lives in the sink and is primed at
 * the boot-to-play hand-off. An unprimed sink never holds more than B frames,
 * so a capacity of B alone would pass every assertion below and the whole L + B
 * rule would go untested. With the lookahead in place the sink reaches exactly
 * L + B at the moment the block is produced.
 *
 * L comes from the fixture.
 */

#include "codecQueues.h"

#include <cstddef>
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

	/* L, supplied by this fixture, and B, the largest host block in 96 kHz
	 * frames. */
	constexpr size_t kLookaheadFrames = 96;
	constexpr size_t kLargestBlock    = 256;
	constexpr size_t kCapacity        = kLookaheadFrames + kLargestBlock;
	constexpr size_t kBlockCount      = 1000;

	/* Every frame carries a distinct value in every slot, derived from its
	 * position in the whole run, so that "pull returned the right frames"
	 * is a statement about content and not about a count. */
	g2::Frame frameFor(const int64_t index)
	{
		g2::Frame frame{};

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			frame.slot[k] = static_cast<int32_t>(
				(index * 16 + static_cast<int64_t>(k)) & 0x7FFFFF);
		}

		return frame;
	}

	bool sameFrame(const g2::Frame& a, const g2::Frame& b)
	{
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			if(a.slot[k] != b.slot[k])
				return false;
		}

		return true;
	}

	bool isZeroFrame(const g2::Frame& frame)
	{
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			if(frame.slot[k] != 0)
				return false;
		}

		return true;
	}

	/* The FIXTURE'S runFrames. One frame out of the source and one frame into
	 * the sink, for each frame of the block, in that order. The RETURN is how
	 * many frames the sink accepted, so a caller can see a short run instead
	 * of inferring one. */
	size_t runFrames(g2::CodecSource& source, g2::CodecSink& sink,
		const size_t frames)
	{
		size_t accepted = 0;

		for(size_t i = 0; i < frames; ++i)
		{
			const g2::Frame frame = source.front();
			source.pop();

			if(sink.push(frame))
				++accepted;
		}

		return accepted;
	}
}

int main()
{
	/* ---------------- 1,000 blocks of 256 frames at a capacity of L + B. */
	{
		g2::CodecSource source(kCapacity);
		g2::CodecSink   sink(kCapacity);

		/* The boot-to-play hand-off leaves the sink holding exactly L frames.
		 * See the file header for why this line is load-bearing. */
		for(size_t i = 0; i < kLookaheadFrames; ++i)
		{
			if(!sink.push(g2::Frame{}))
			{
				printf("FAIL the sink could not be primed with the lookahead "
					"at frame %zu\n", i);
				++failures;
				break;
			}
		}

		checkEqual(sink.size(), kLookaheadFrames,
			"the primed sink holds exactly the lookahead");

		std::vector<g2::Frame> pulled(kLargestBlock);

		int64_t pushIndex = 0;
		int64_t pullIndex = 0;

		size_t highWaterSink   = 0;
		size_t highWaterSource = 0;

		for(size_t block = 0; block < kBlockCount && failures == 0; ++block)
		{
			/* push */
			for(size_t i = 0; i < kLargestBlock; ++i)
			{
				if(!source.push(frameFor(pushIndex)))
				{
					printf("FAIL push was refused at frame %lld of block "
						"%zu\n", static_cast<long long>(pushIndex), block);
					++failures;
					break;
				}

				++pushIndex;
			}

			if(source.size() > highWaterSource)
				highWaterSource = source.size();

			/* runFrames */
			const size_t accepted = runFrames(source, sink, kLargestBlock);

			if(accepted != kLargestBlock)
			{
				printf("FAIL the sink accepted %zu of the %zu frames of block "
					"%zu\n", accepted, kLargestBlock, block);
				++failures;
				break;
			}

			if(sink.size() > highWaterSink)
				highWaterSink = sink.size();

			/* pull */
			const size_t taken = sink.pull(pulled.data(), kLargestBlock);

			if(taken != kLargestBlock)
			{
				printf("FAIL pull returned %zu of the %zu frames of block "
					"%zu\n", taken, kLargestBlock, block);
				++failures;
				break;
			}

			/* And the frames are the right ones. The first L frames out are
			 * the primed silence; every later frame is the one pushed L
			 * frames earlier, which is the lookahead delay the capacity rule
			 * exists to hold. */
			for(size_t i = 0; i < kLargestBlock; ++i)
			{
				const bool correct =
					pullIndex < static_cast<int64_t>(kLookaheadFrames)
						? isZeroFrame(pulled[i])
						: sameFrame(pulled[i],
							frameFor(pullIndex
								- static_cast<int64_t>(kLookaheadFrames)));

				if(!correct)
				{
					printf("FAIL the frame pulled at position %lld of block "
						"%zu is not the one the lookahead delay names\n",
						static_cast<long long>(pullIndex), block);
					++failures;
					break;
				}

				++pullIndex;
			}
		}

		checkEqual(static_cast<uint64_t>(pushIndex),
			kBlockCount * kLargestBlock, "every frame of every block was "
			"pushed");
		checkEqual(static_cast<uint64_t>(pullIndex),
			kBlockCount * kLargestBlock, "every frame of every block was "
			"pulled");

		/* The three counters are zero. Any one of them above zero is a defect
		 * report and not a tolerance. */
		checkEqual(source.overflowFrames(), 0u,
			"overflowFrames is zero: the source never refused a frame");
		checkEqual(source.starvedFrames(), 0u,
			"starvedFrames is zero: no quantum consumed a frame the host had "
			"not supplied");
		checkEqual(sink.droppedFrames(), 0u,
			"droppedFrames is zero: the sink never refused a frame");
		checkEqual(sink.underflowFrames(), 0u,
			"underflowFrames is zero: every pull returned the whole request");

		/* The capacity is exercised to its limit and not beyond. The sink
		 * really does reach L + B, which is what says the rule is necessary
		 * rather than generous. */
		checkEqual(highWaterSink, kCapacity,
			"the sink reaches EXACTLY L + B frames. A smaller high-water mark "
			"would mean this run never tested the capacity rule at all.");
		checkEqual(highWaterSource, kLargestBlock,
			"the source reaches exactly B frames, because push delivers a "
			"whole block before runFrames consumes any of it");
	}

	/* ---------------- NEGATIVE CASE 1: an under-sized CodecSource drives
	 * overflowFrames above zero. */
	{
		const size_t undersized = kLargestBlock / 2;

		g2::CodecSource source(undersized);
		g2::CodecSink   sink(kCapacity);

		size_t refused = 0;

		for(size_t i = 0; i < kLargestBlock; ++i)
		{
			if(!source.push(frameFor(static_cast<int64_t>(i))))
				++refused;
		}

		checkEqual(refused, kLargestBlock - undersized,
			"an under-sized source refuses every frame above its capacity");
		check(source.overflowFrames() > 0,
			"an under-sized CodecSource drives overflowFrames ABOVE ZERO");
		checkEqual(source.overflowFrames(), kLargestBlock - undersized,
			"every refused input frame is counted");

		(void) sink;
	}

	/* ---------------- NEGATIVE CASE 2: an under-sized CodecSink drives both
	 * droppedFrames and underflowFrames above zero.
	 *
	 * This is the quadrant an under-sized sink capacity actually lands in, and
	 * an earlier design draft had no counter for the second half of it. */
	{
		const size_t undersized = kLookaheadFrames + 16;

		g2::CodecSource source(kCapacity);
		g2::CodecSink   sink(undersized);

		for(size_t i = 0; i < kLookaheadFrames; ++i)
			sink.push(g2::Frame{});

		for(size_t i = 0; i < kLargestBlock; ++i)
			source.push(frameFor(static_cast<int64_t>(i)));

		const size_t accepted = runFrames(source, sink, kLargestBlock);

		checkEqual(accepted, undersized - kLookaheadFrames,
			"an under-sized sink accepts only what its capacity leaves free "
			"above the lookahead");
		check(sink.droppedFrames() > 0,
			"an under-sized CodecSink drives droppedFrames ABOVE ZERO");

		std::vector<g2::Frame> pulled(kLargestBlock);

		const size_t taken = sink.pull(pulled.data(), kLargestBlock);

		checkEqual(taken, undersized,
			"the under-sized sink can supply only what it holds");
		check(sink.underflowFrames() > 0,
			"an under-sized CodecSink drives underflowFrames ABOVE ZERO too");
		checkEqual(sink.underflowFrames(), kLargestBlock - undersized,
			"the shortfall is counted frame by frame");
	}

	/* ---------------- the CAPACITY RULE is TIGHT, not GENEROUS.
	 *
	 * A sink one FRAME SHORT of L + B drops a frame on the first block. That
	 * is what says L + B is the answer and L + B - 1 is not, and it is the
	 * assertion an over-generous capacity would hide. */
	{
		g2::CodecSource source(kCapacity);
		g2::CodecSink   sink(kCapacity - 1);

		for(size_t i = 0; i < kLookaheadFrames; ++i)
			sink.push(g2::Frame{});

		for(size_t i = 0; i < kLargestBlock; ++i)
			source.push(frameFor(static_cast<int64_t>(i)));

		const size_t accepted = runFrames(source, sink, kLargestBlock);

		checkEqual(accepted, kLargestBlock - 1,
			"a sink one frame short of L + B accepts one frame fewer than the "
			"block");
		checkEqual(sink.droppedFrames(), 1u,
			"a sink one frame short of L + B drops exactly one frame on the "
			"first block");
	}

	/* ---------------- and A SINK SIZED L + framesPerQuantum is the NAMED
	 * DEFECT.
	 *
	 * With framesPerQuantum fixed at 1 that capacity is L + 1: the scheduler
	 * runs one quantum for each host block and then stops, and the plugin
	 * emits no audio at all. */
	{
		g2::CodecSource source(kCapacity);
		g2::CodecSink   sink(kLookaheadFrames + 1);

		for(size_t i = 0; i < kLookaheadFrames; ++i)
			sink.push(g2::Frame{});

		for(size_t i = 0; i < kLargestBlock; ++i)
			source.push(frameFor(static_cast<int64_t>(i)));

		const size_t accepted = runFrames(source, sink, kLargestBlock);

		checkEqual(accepted, 1u,
			"a sink sized L + framesPerQuantum accepts ONE frame of a whole "
			"host block");
		checkEqual(sink.droppedFrames(), kLargestBlock - 1,
			"every other frame of the block is dropped");
	}

	if(failures != 0)
	{
		printf("t0_codec_capacity: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_codec_capacity: all cases passed (%zu blocks of %zu frames at a "
		"capacity of %zu = L %zu + B %zu)\n", kBlockCount, kLargestBlock,
		kCapacity, kLookaheadFrames, kLargestBlock);
	return 0;
}
