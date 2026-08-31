/* scheduler.cpp -- the Scheduler factory, the construction rejections, and the
 * quantum.
 *
 * `Scheduler::create` is the single rejection point. Every rejectable value
 * arrives through `Scheduler::Config`, so no other site has to repeat any of
 * these comparisons and no later object can be built from a Config that failed
 * one.
 *
 * A Config can fail more than one row, and then the reported status is
 * whichever comparison runs first. Testing the lookahead bound early was
 * rejected because the bound reads a value the DSP-count row has not yet
 * vetted: `D_chain` is `(N - 1) * hopFrames` over the unsigned members
 * ChainAdapter holds, so a count of 0 makes the subtraction WRAP, the chain
 * delay enormous and the bound exceeded -- and a factory that tested the bound
 * first would answer BadLookahead for a Config whose actual defect is the
 * count. Running the table in order means every value the later rows read has
 * already been vetted by an earlier one. A caller reading one status back
 * learns the FIRST defect and not the only one.
 *
 * No exception and no assertion anywhere in this file: a release build removes
 * an assertion, so the rejections have to survive NDEBUG. The return value and
 * the status out-param are the whole observable.
 */

#include "scheduler.h"

#include "board.h"
#include "executor.h"

#include <cstdint>

namespace g2
{
	/* The DSP job body lives in dspJob.cpp and no header declares it. The
	 * declaration is here rather than in a new header because this is its only
	 * production caller: the job array below is the one place a pointer to it is
	 * taken. */
	void dspJob(JobContext* _ctx) noexcept;

	namespace
	{
		/* Above this the framework silently truncates the reported latency and
		 * logs that audio will be out of sync. The bound is on the
		 * SUM, and `D_codec` is written as a named term rather than folded away
		 * so that a future decision to model the converter has one place to
		 * change. */
		constexpr uint64_t kMaxTotalLookaheadFrames = 16384;
		constexpr uint64_t kDelayCodecFrames        = 0;

		/* The job array holds the DSP contexts only and is fixed at
		 * `kJobCount`; the panel and the MCU run serially in the Scheduler,
		 * outside the Executor. Every other count is rejected, including the
 * 4-DSP machine that is a real configuration. */
		constexpr unsigned kFixedDspCount = static_cast<unsigned>(kJobCount);

		/* A value above 1 would give one transmit callback per frame with a
		 * single swap point outside the run phase, so each write would
		 * overwrite the last and the mailbox would drop all but the final frame
		 * of every quantum. */
		constexpr unsigned kFixedFramesPerQuantum = 1;
	}

	std::unique_ptr<Scheduler> Scheduler::create(const Config& _config, Executor& _executor, Board& _board,
		Status& _outStatus)
	{
		/* Neither reference is read before the table has run, and neither is
		 * validated: a reference cannot be null, and whether the referent
		 * outlives the Scheduler is a lifetime rule this factory cannot
		 * check. */

		if(_config.framesPerQuantum != kFixedFramesPerQuantum)
		{
			_outStatus = Status::BadFramesPerQuantum;
			return nullptr;
		}

		/* A hop of zero collapses the hop whatever the override says. Every
		 * mailbox is a delay line of `hopFrames + 1` frames, so a frame written
		 * in one quantum would be taken back in that same quantum and the
 * rule that no job may observe another job's state would break inside
		 * the very object the override exists to exercise. */
		if(_config.hopFrames == 0)
		{
			_outStatus = Status::BadHopFrames;
			return nullptr;
		}

		/* The divider is a DIVISOR. dspContext.h's comment on that member
		 * states this rejection as the reason its modulo cannot divide by
		 * zero, so the override must not be able to hand a zero through. */
		if(_config.secondBusFrameDivider == 0)
		{
			_outStatus = Status::BadDivider;
			return nullptr;
		}

		/* The two equality rows. `Config::secondBusFrameDivider` and
		 * `Config::hopFrames` are the values every consumer actually reads, and
		 * without these a Scheduler built with a divider of 1 against a build
		 * constant of 4 was legal -- a golden render taken through it would pass
		 * the timebase gate while the manifest recorded 4.
		 *
		 * The override is the only way past them, and it does not reach the two
		 * zero rows above. */
		if(!_config.testOverride)
		{
			if(_config.secondBusFrameDivider != G2_SECOND_BUS_FRAME_DIVIDER)
			{
				_outStatus = Status::BadDivider;
				return nullptr;
			}

			if(_config.hopFrames != G2_CHAIN_HOP_FRAMES)
			{
				_outStatus = Status::BadHopFrames;
				return nullptr;
			}
		}

		if(_config.dspCount != kFixedDspCount)
		{
			_outStatus = Status::BadDspCount;
			return nullptr;
		}

		if(_config.dspRate.den == 0 || _config.mcuRate.den == 0)
		{
			_outStatus = Status::BadRational;
			return nullptr;
		}

		/* Read only after the DSP-count row has vetted `dspCount`. The
		 * subtraction is unsigned and its operand types are ChainAdapter's, so
		 * the widening below does not rescue a count of zero -- the wrap happens
		 * first, in `unsigned`, and only the order above keeps it unreachable.
		 *
		 * The sum is accumulated in 64 bits and not in `unsigned`, and that is a
		 * decision rather than habit. A 32-bit sum of a wrapped delay and a
		 * small lookahead wraps a SECOND time and lands back under the bound, so
		 * a Config that ought to be refused would be accepted here -- a check
		 * that passes for the wrong reason, and one whose silence would look
		 * exactly like a correct answer.
		 *
		 * A wrapped delay is not the only way past 32 BITS. The count row bounds
		 * `dspCount`, but nothing bounds `hopFrames` or `lookaheadFrames` once
		 * the override is taken, so those two terms alone carry the sum over 32
		 * bits while every earlier row is satisfied and no subtraction has
		 * wrapped. Narrowing this sum accepts such a Config.
		 *
		 * What the width does not establish: it bounds neither term. It holds
		 * the widest sum two 32-bit members can produce here and nothing beyond
		 * that, so widening `Config`'s own members puts this line back in
		 * question rather than inheriting the guarantee. */
		{
			const uint64_t chainDelay = static_cast<uint64_t>(_config.dspCount - 1u) * _config.hopFrames;
			const uint64_t total      = chainDelay + kDelayCodecFrames + _config.lookaheadFrames;

			if(total > kMaxTotalLookaheadFrames)
			{
				_outStatus = Status::BadLookahead;
				return nullptr;
			}
		}

		/* The codec queues carry the capacity `lookaheadFrames + B`, so a B of
		 * zero sizes them by the lookahead alone. */
		if(_config.maxHostBlockFrames == 0)
		{
			_outStatus = Status::BadMaxHostBlock;
			return nullptr;
		}

		/* `g_useJIT` is a `static constexpr`, so the second half folds at
		 * compile time. */
		if(_config.backend != Backend::Jit || !dsp56k::g_useJIT)
		{
			_outStatus = Status::BadBackend;
			return nullptr;
		}

		_outStatus = Status::Ok;
		return std::unique_ptr<Scheduler>(new Scheduler(_config, _executor, _board));
	}

	/* The Scheduler drives the Board's own DSP set and constructs none.
	 *
	 *   1. The Board's set is the BRIDGED one -- board.cpp holds the only
	 *      production call of attachHdi08Bridges -- and a bridge carries the
	 *      landed flag the run gate borrows.
	 *   2. attachHdi08Bridges REFUSES a second attach, so a second set could
	 *      not be bridged after the fact.
	 *   3. dspSet.h answers NULL for a slot with no bridge, and the run gate
	 *      reads NULL as NOT LANDED -- so every slot of a Scheduler-owned set
	 *      would stay shut for the life of the program while every check here
	 *      stayed green.
	 *
	 * The chain callbacks are installed here and nowhere else, because this is
	 * the one object that holds BOTH ends of that wire: the ChainAdapter it
	 * owns and the set it drives. The Board attaches the HDI08 bridges and does
	 * not touch the chain callbacks. */
	Scheduler::Scheduler(const Config& _config, Executor& _executor, Board& _board)
		: m_executor(_executor)
		, m_board(_board)
		, m_trace(_config.trace)
		, m_chain(_config.dspCount, _config.hopFrames, _config.secondBusTopology,
			_config.secondBusFrameDivider)
		, m_mcuRate(_config.mcuRate)
	{
		DspSet& set = _board.dspSet();

		/* `dspCount` has already been vetted against kJobCount by the table
		 * above, so the two bounds cannot disagree here. */
		for(unsigned i = 0; i < static_cast<unsigned>(kJobCount); ++i)
		{
			DspContext& c = m_contexts[i];

			c.base.fault            = JobFault::None;
			c.position              = i;
			c.rate                  = _config.dspRate;

			/* The accumulator, the cycle debt and the long-dispatch counter
			 * are zero, and the `{}` on m_contexts is what makes them zero --
			 * this loop deliberately writes none of the three. The debt loop
			 * reads the accumulator before it ever writes one, so the zero is
			 * required; writing it here as well would read like the guarantee
			 * without being it, and no mutation of such a write could go
			 * red. */

			/* Borrowed from the Board's set, one slot for each position. */
			c.dsp                   = &set.dsp(i);
			c.audioEsai             = &set.peripherals(i).getEsai();
			c.secondEsai            = &set.peripherals(i).getEsai1();

			c.frameIndex            = m_frameIndex;

			/* The run gate's pointer, borrowed and never copied. The producer
			 * sets its own flag when the download completes, and a value taken
			 * here could never see it. NULL for a slot with no bridge, which
			 * the gate already reads as NOT LANDED. */
			c.programLanded         = set.programLanded(i);

			c.secondBusFrameDivider = _config.secondBusFrameDivider;

			m_jobs[i].fn  = &dspJob;
			m_jobs[i].ctx = &c.base;
		}

		/* Last, and the order is load-bearing. This call publishes callbacks
		 * that capture `this` into ESAIs the Board owns, so every member the
		 * callbacks reach must already hold its final value when it runs.
		 * Publishing a half-built object is the one ordering error here that
		 * no compiler reports. */
		attachChainCallbacks(m_chain, set);
	}

	void Scheduler::mark(const TracePhase _phase, const uint64_t _frameIndex) const noexcept
	{
		if(m_trace != nullptr)
			m_trace->onPhase(_phase, _frameIndex);
	}

	/* One quantum, and this is its only site.
	 *
	 *     swap     ChainAdapter::advanceAll(frameIndex)
	 *     ingress  ChainAdapter::injectCodecSource(frame)      play regime only
	 *     run      0  Panel::tick(frameIndex)
	 *              1  Board::tickSofIfDue(frameIndex), then Board::runMcu(...)
	 *              2  DSP 0 .. DSP 7, ascending
	 *     egress   ChainAdapter::extractCodecSink(frame)       play regime only
	 *
	 * The two play-only phases are not this function's. This class has no
 * regime member, so it is the boot machine by construction, and the boot
 * regime runs the swap and the run phase only. The trace of one quantum is
 * therefore five records: Swap, Panel, Sof, Mcu, Dsp.
 *
 * The panel and the MCU run serially here, outside the Executor, which is
 * why the job array holds the eight DSP contexts and nothing else.
 *
 * No allocation happens in this function. The adapter, the contexts and the
	 * job array are all built once, at construction. */
	void Scheduler::runFrames(const size_t _frames) noexcept
	{
		for(size_t f = 0; f < _frames; ++f)
		{
			const uint64_t frameIndex = m_frameIndex;

			/* The frame index reaches every context before Executor::run, and
			 * no job writes it. It is a copy and not a pointer, so a job cannot
			 * observe another job's state through it. */
			for(auto& c : m_contexts)
				c.frameIndex = frameIndex;

			mark(TracePhase::Swap, frameIndex);
			m_chain.advanceAll(frameIndex);

			mark(TracePhase::Panel, frameIndex);
			m_board.panel().tick(frameIndex);

			/* Immediately before runMcu. The Board owns the due test and this
			 * call is unconditional. */
			mark(TracePhase::Sof, frameIndex);
			m_board.tickSofIfDue(frameIndex);

			/* The allocation alone, and the argument rule is provisional.
			 * `wantCycles` is the rational allocation for ONE frame: nothing
			 * is subtracted from it, nothing is carried back and the return
			 * value is discarded. No check here asserts anything about the
			 * MCU's cycle accounting. */
			mark(TracePhase::Mcu, frameIndex);
			(void) m_board.runMcu(alloc(m_mcuRate, &m_mcuAcc));   /* PROVISIONAL */

			mark(TracePhase::Dsp, frameIndex);
			m_executor.run(m_jobs, kJobCount);

			++m_frameIndex;
		}
	}
}
