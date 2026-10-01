/* Two things, which fail in different ways on purpose.
 *
 * The surface. The check takes the address of every declared method on both
 * queues, through its fully qualified member-function-pointer type. That
 * spelling is what makes a renamed or re-signed method a compile error rather
 * than a silent match against something else, and a missing definition a link
 * error, because taking the address of a member function odr-uses it.
 *
 * The behavioural property: CodecSink::push refuses when full
 * and the frame already in the queue is unchanged afterwards. Overwriting would
 * silently discard a frame the host has already been told to expect, which
 * changes the real latency mid-session while the reported figure stays
 * constant. Refusing makes it observable, and droppedFrames() > 0 is a defect
 * report and not a tolerance.
 */

#include "codecQueues.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

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

	/* A frame whose every slot carries a distinct value derived from one seed,
	 * so that "the frame already in the queue is unchanged" is a statement
	 * about all eight slots and not about one. */
	g2::Frame frameFor(const int32_t seed)
	{
		g2::Frame frame{};
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			frame.slot[k] = seed * 16 + static_cast<int32_t>(k);
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
}

/* ================ the surface
 *
 * Every declared method of both queues, by its fully qualified
 * member-function-pointer type. Remove a parameter, change a return type, drop
 * a const or a noexcept, or rename a method, and the initialiser below fails
 * to compile. Leave a method declared and undefined and main() fails to link.
 */

static constexpr bool (g2::CodecSource::*kSourcePush)(const g2::Frame&) noexcept
	= &g2::CodecSource::push;
static constexpr const g2::Frame& (g2::CodecSource::*kSourceFront)() const noexcept
	= &g2::CodecSource::front;
static constexpr void (g2::CodecSource::*kSourcePop)() noexcept
	= &g2::CodecSource::pop;
static constexpr size_t (g2::CodecSource::*kSourceSize)() const noexcept
	= &g2::CodecSource::size;
static constexpr size_t (g2::CodecSource::*kSourceCapacity)() const noexcept
	= &g2::CodecSource::capacity;
static constexpr uint64_t (g2::CodecSource::*kSourceOverflow)() const noexcept
	= &g2::CodecSource::overflowFrames;
static constexpr uint64_t (g2::CodecSource::*kSourceStarved)() const noexcept
	= &g2::CodecSource::starvedFrames;

static constexpr bool (g2::CodecSink::*kSinkPush)(const g2::Frame&) noexcept
	= &g2::CodecSink::push;
static constexpr size_t (g2::CodecSink::*kSinkPull)(g2::Frame*, size_t) noexcept
	= &g2::CodecSink::pull;
static constexpr size_t (g2::CodecSink::*kSinkSize)() const noexcept
	= &g2::CodecSink::size;
static constexpr size_t (g2::CodecSink::*kSinkCapacity)() const noexcept
	= &g2::CodecSink::capacity;
static constexpr uint64_t (g2::CodecSink::*kSinkDropped)() const noexcept
	= &g2::CodecSink::droppedFrames;
static constexpr uint64_t (g2::CodecSink::*kSinkUnderflow)() const noexcept
	= &g2::CodecSink::underflowFrames;

/* Both constructors take the capacity and nothing else, and both are explicit,
 * so a bare integer cannot become a queue by accident. */
static_assert(!std::is_convertible_v<size_t, g2::CodecSource>,
	"CodecSource's constructor is explicit.");
static_assert(!std::is_convertible_v<size_t, g2::CodecSink>,
	"CodecSink's constructor is explicit.");

int main()
{
	/* The capacity is lookaheadFrames + B for both queues, and both figures
	 * come from this fixture. B is the largest host block in 96 kHz frames and
	 * the Device cannot see it: synthLib::Device has no prepareToPlay and no
	 * block-size accessor, and synthLib::Plugin::setBlockSize keeps the value
	 * private and never forwards it. Scheduler::Config::maxHostBlockFrames is
	 * what supplies it in the shipping path.
	 *
	 * The two queues are the same size because they are symmetric: push
	 * delivers a whole block before runFrames consumes any of it, and
	 * runFrames produces a whole block before pull takes any of it. A sink
	 * sized lookaheadFrames + framesPerQuantum makes the scheduler run one
	 * quantum for each host block and then stop -- a plugin that emits no
	 * audio at all. That is the defect the L + B capacity closes. */
	const size_t lookaheadFrames = 96u;
	const size_t largestBlock    = 256u;
	const size_t capacityFrames  = lookaheadFrames + largestBlock;

	/* ---------------- the surface really is callable.
	 *
	 * Every pointer above is called through, so a declared-and-undefined
	 * method is a link error rather than a pointer nobody used. */
	{
		g2::CodecSource source(capacityFrames);
		g2::CodecSink   sink(capacityFrames);

		g2::CodecSource* const s = &source;
		g2::CodecSink*   const k = &sink;

		checkEqual((s->*kSourceCapacity)(), capacityFrames,
			"CodecSource::capacity is the capacity it was built with");
		checkEqual((k->*kSinkCapacity)(), capacityFrames,
			"CodecSink::capacity is the capacity it was built with");

		checkEqual((s->*kSourceSize)(), 0u, "a new CodecSource is empty");
		checkEqual((k->*kSinkSize)(), 0u, "a new CodecSink is empty");

		check((s->*kSourcePush)(frameFor(1)),
			"CodecSource::push accepts a frame into an empty queue");
		check(sameFrame((s->*kSourceFront)(), frameFor(1)),
			"CodecSource::front returns the frame that was pushed");
		(s->*kSourcePop)();
		checkEqual((s->*kSourceSize)(), 0u,
			"CodecSource::pop removes the frame");

		check((k->*kSinkPush)(frameFor(2)),
			"CodecSink::push accepts a frame into an empty queue");

		g2::Frame pulled[1];
		checkEqual((k->*kSinkPull)(pulled, 1u), 1u,
			"CodecSink::pull returns the frame count it took");
		check(sameFrame(pulled[0], frameFor(2)),
			"CodecSink::pull returns the frame that was pushed");

		checkEqual((s->*kSourceOverflow)(), 0u,
			"CodecSource::overflowFrames is zero on a queue that never "
			"refused");
		checkEqual((s->*kSourceStarved)(), 0u,
			"CodecSource::starvedFrames is zero on a queue that never "
			"starved");
		checkEqual((k->*kSinkDropped)(), 0u,
			"CodecSink::droppedFrames is zero on a queue that never refused");
		checkEqual((k->*kSinkUnderflow)(), 0u,
			"CodecSink::underflowFrames is zero on a queue that always "
			"supplied a whole request");
	}

	/* ---------------- CodecSink::push refuses when full and never overwrites.
	 *
	 * The queue is filled with frames
	 * whose contents are known, a further push is made, and every frame
	 * already in the queue is asserted unchanged afterwards -- not the oldest
	 * alone, because an overwrite of any one of them is the same defect. */
	{
		g2::CodecSink sink(capacityFrames);

		for(size_t i = 0; i < capacityFrames; ++i)
		{
			if(!sink.push(frameFor(static_cast<int32_t>(i))))
			{
				printf("FAIL the sink refused frame %zu of a capacity of "
					"%zu\n", i, capacityFrames);
				++failures;
				break;
			}
		}

		checkEqual(sink.size(), capacityFrames, "the sink is full");
		checkEqual(sink.droppedFrames(), 0u,
			"a sink filled exactly to its capacity dropped nothing");

		/* The refusal. */
		const g2::Frame intruder = frameFor(999);

		check(!sink.push(intruder),
			"CodecSink::push REFUSES when the queue is full");

		checkEqual(sink.droppedFrames(), 1u,
			"a refused frame is counted, because droppedFrames() above zero "
			"is a defect report");
		checkEqual(sink.size(), capacityFrames,
			"a refused push does not change the queue's size");

		/* And nothing in the queue moved. Every frame is pulled and held
		 * against what was pushed. */
		for(size_t i = 0; i < capacityFrames; ++i)
		{
			g2::Frame out[1];

			if(sink.pull(out, 1u) != 1u)
			{
				printf("FAIL the sink could not supply frame %zu\n", i);
				++failures;
				break;
			}

			if(!sameFrame(out[0], frameFor(static_cast<int32_t>(i))))
			{
				printf("FAIL the sink's frame %zu changed after a refused "
					"push\n", i);
				++failures;
				break;
			}

			if(sameFrame(out[0], intruder))
			{
				printf("FAIL the refused frame reached position %zu of the "
					"queue\n", i);
				++failures;
				break;
			}
		}

		checkEqual(sink.size(), 0u,
			"every frame the sink accepted comes back out of it");
		checkEqual(sink.droppedFrames(), 1u,
			"the drop count is not cleared by a pull");
	}

	/* ---------------- CodecSource::push refuses when full, and counts it.
	 *
	 * A refused frame is host audio input and it is dropped. There is no retry
	 * and no recovery, and the audible consequence is a gap of that many
	 * frames in the input path. The capacity rule makes it unreachable in a
	 * correct build, which is why it is counted rather than handled, but the
	 * behaviour is asserted so that nobody reads "unreachable" as
	 * "harmless". */
	{
		g2::CodecSource source(capacityFrames);

		for(size_t i = 0; i < capacityFrames; ++i)
		{
			if(!source.push(frameFor(static_cast<int32_t>(i))))
			{
				printf("FAIL the source refused frame %zu of a capacity of "
					"%zu\n", i, capacityFrames);
				++failures;
				break;
			}
		}

		check(!source.push(frameFor(999)),
			"CodecSource::push refuses when the queue is full");
		checkEqual(source.overflowFrames(), 1u,
			"a refused input frame is counted");
		checkEqual(source.size(), capacityFrames,
			"a refused push does not change the source's size");

		/* And the queue is unchanged, in order. */
		for(size_t i = 0; i < capacityFrames; ++i)
		{
			if(!sameFrame(source.front(), frameFor(static_cast<int32_t>(i))))
			{
				printf("FAIL the source's frame %zu is not the one that was "
					"pushed\n", i);
				++failures;
				break;
			}
			source.pop();
		}

		checkEqual(source.size(), 0u, "the source empties in order");
	}

	/* ---------------- an empty source returns a zero frame and counts the
	 * starve.
	 *
	 * The counter exists because a starve and an overflow are the two
	 * symmetric failures of the input side. */
	{
		g2::CodecSource source(capacityFrames);

		check(isZeroFrame(source.front()),
			"an empty CodecSource returns a ZERO frame, not stale data");

		checkEqual(source.starvedFrames(), 0u,
			"reading front alone does not count a starve");

		source.pop();

		checkEqual(source.starvedFrames(), 1u,
			"consuming from an empty source counts one starved frame");

		source.pop();
		source.pop();

		checkEqual(source.starvedFrames(), 3u,
			"every consumed zero frame is counted");

		/* A pop with a frame in the queue counts nothing. */
		check(source.push(frameFor(4)), "the source accepts a frame again");
		source.pop();

		checkEqual(source.starvedFrames(), 3u,
			"a pop that really consumed a frame counts no starve");
	}

	/* ---------------- a short pull is counted as an underflow.
	 *
	 * underflowFrames is the number by which a pull's return fell short of its
	 * request. It is the sink's under-supply counter: without it, the quadrant
	 * an under-sized sink capacity lands in is the one nothing watches. */
	{
		g2::CodecSink sink(capacityFrames);

		check(sink.push(frameFor(1)), "the sink accepts one frame");
		check(sink.push(frameFor(2)), "the sink accepts a second frame");

		/* The buffer is pre-filled with a sentinel, and that is not tidiness.
		 * An uninitialised buffer that happens to be zero makes the silence
		 * assertion below pass whether or not pull() writes anything -- a
		 * check that cannot fail. This was measured: with the zeroing removed
		 * from pull(), the case passed until the buffer carried a sentinel. */
		g2::Frame out[8];
		for(size_t i = 0; i < 8u; ++i)
			out[i] = frameFor(-1);

		checkEqual(sink.pull(out, 8u), 2u,
			"a pull that cannot be filled returns what it took");
		checkEqual(sink.underflowFrames(), 6u,
			"the shortfall is counted, frame by frame");

		check(sameFrame(out[0], frameFor(1)) && sameFrame(out[1], frameFor(2)),
			"a short pull still returns the frames it had, in order");

		/* The frames it could not supply read as silence. A consumer receives
		 * the whole buffer it asked for, so the part that was not filled must
		 * not carry whatever was there before. */
		for(size_t i = 2; i < 8u; ++i)
		{
			if(!isZeroFrame(out[i]))
			{
				printf("FAIL the unfilled frame %zu of a short pull is not "
					"silence\n", i);
				++failures;
				break;
			}
		}

		for(size_t i = 0; i < 8u; ++i)
			out[i] = frameFor(-1);

		checkEqual(sink.pull(out, 4u), 0u,
			"a pull from an empty sink takes nothing");

		for(size_t i = 0; i < 4u; ++i)
		{
			if(!isZeroFrame(out[i]))
			{
				printf("FAIL frame %zu of a wholly unfilled pull is not "
					"silence\n", i);
				++failures;
				break;
			}
		}
		checkEqual(sink.underflowFrames(), 10u,
			"the shortfall of a wholly unfilled pull is the whole request");
	}

	/* ---------------- the ring really wraps.
	 *
	 * Both queues are driven well past their capacity in a push-and-consume
	 * cycle, so the index arithmetic is exercised rather than assumed. A ring
	 * that wrapped wrongly would return the right count and the wrong frame,
	 * which is why every frame is held against its seed. */
	{
		g2::CodecSource source(capacityFrames);
		g2::CodecSink   sink(capacityFrames);

		for(int32_t i = 0; i < 10000; ++i)
		{
			check(source.push(frameFor(i)), "the source accepts a frame");
			check(sameFrame(source.front(), frameFor(i)),
				"the source returns the frame that was pushed");
			source.pop();

			check(sink.push(frameFor(i)), "the sink accepts a frame");

			g2::Frame out[1];
			checkEqual(sink.pull(out, 1u), 1u, "the sink supplies one frame");
			check(sameFrame(out[0], frameFor(i)),
				"the sink returns the frame that was pushed");

			if(failures != 0)
				break;
		}

		checkEqual(source.overflowFrames(), 0u,
			"a source that never filled up refused nothing");
		checkEqual(source.starvedFrames(), 0u,
			"a source that always had a frame starved never");
		checkEqual(sink.droppedFrames(), 0u,
			"a sink that never filled up refused nothing");
		checkEqual(sink.underflowFrames(), 0u,
			"a sink that always supplied the whole request underflowed never");
	}

	if(failures != 0)
	{
		printf("t0_codec_queue_surface: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_codec_queue_surface: all cases passed\n");
	return 0;
}
