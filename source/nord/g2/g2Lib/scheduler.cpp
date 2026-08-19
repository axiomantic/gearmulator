/* scheduler.cpp -- the Scheduler factory, the construction rejections, and the
 * quantum. Tasks SCH-18 and SCH-19. Design sections 13.5, 13.10 rule 4,
 * 13.4.5, 13.6, 13.6.2 and 13.10.5.
 *
 * SCH-18 OWNS THE FILE AND THE TABLE; SCH-19 OWNS WHAT IS BUILT ONCE THE TABLE
 * HAS PASSED -- the constructor below `create`, and `runFrames`.
 *
 * `Scheduler::create` is the SINGLE REJECTION POINT. Every rejectable value
 * arrives through `Scheduler::Config`, so no other site has to repeat any of
 * these comparisons and no later object can be built from a Config that failed
 * one.
 *
 * THE ORDER OF THE COMPARISONS IS THE ORDER OF THE PLAN'S REJECTION TABLE, AND
 * THAT IS A DECISION RATHER THAN AN ACCIDENT.
 *
 *   A Config can fail more than one row, and then the reported status is
 *   whichever comparison runs first. The alternative considered was to test the
 *   lookahead bound early, on the ground that it is the most expensive mistake
 *   to ship; it was rejected because the bound reads a value the DSP-count row
 *   has not yet vetted. `D_chain` is `(N - 1) * hopFrames` over the unsigned
 *   members ChainAdapter holds, so a count of 0 makes the subtraction WRAP, the
 *   chain delay enormous and the bound exceeded -- and a factory that tested the
 *   bound first would answer BadLookahead for a Config whose actual defect is
 *   the count. Running the table in order means every value the later rows read
 *   has already been vetted by an earlier one.
 *
 *   WHAT THAT DOES NOT ESTABLISH: it fixes the reported status for a Config
 *   that fails several rows, and it says nothing about which single row a
 *   caller's Config was meant to fail. A caller reading one status back learns
 *   the FIRST defect and not the only one.
 *
 * NO EXCEPTION AND NO ASSERTION ANYWHERE IN THIS FILE. Design section 13.10
 * rule 2 forbids throwing, and a release build removes an assertion, so the
 * rejections have to survive NDEBUG. The return value and the status out-param
 * are the whole observable.
 */

#include "scheduler.h"

#include "board.h"
#include "executor.h"

#include <cstdint>

namespace g2
{
	/* SCH-11 defines the DSP job body in dspJob.cpp and no header declares it.
	 * The declaration is here rather than in a new header because this is its
	 * only production caller: the job array below is the one place a pointer to
	 * it is taken. */
	void dspJob(JobContext* _ctx) noexcept;

	namespace
	{
		/* Above this the framework silently truncates the reported latency and
		 * logs that audio will be out of sync. §13.6.2. The bound is on the
		 * SUM, and `D_codec` is written as a named term rather than folded away
		 * so that a future decision to model the converter has one place to
		 * change. */
		constexpr uint64_t kMaxTotalLookaheadFrames = 16384;
		constexpr uint64_t kDelayCodecFrames        = 0;

		/* The job array holds the DSP contexts only and is fixed at
		 * `kJobCount`; the panel and the MCU run serially in the Scheduler,
		 * outside the Executor. Every other count is rejected, including the
		 * 4-DSP machine BRD-15 records as a real configuration. */
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
		/* NEITHER IS READ BEFORE THE TABLE HAS RUN. SCH-18 rejects on the Config
		 * alone, and neither reference is validated: a reference cannot be
		 * null, and whether the referent outlives the Scheduler is a lifetime
		 * rule this factory cannot check. SCH-19's constructor, below the
		 * table, is where both are wired in. */

		if(_config.framesPerQuantum != kFixedFramesPerQuantum)
		{
			_outStatus = Status::BadFramesPerQuantum;
			return nullptr;
		}

		/* A hop of zero collapses the hop whatever the override says. Every
		 * mailbox is a delay line of `hopFrames + 1` frames, so a frame written
		 * in one quantum would be taken back in that same quantum and SCH-19's
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
		 * THE OVERRIDE IS THE ONLY WAY PAST THEM, and it does not reach the two
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
		 * THE SUM IS ACCUMULATED IN 64 BITS AND NOT IN `unsigned`, and that is a
		 * decision rather than habit. A 32-bit sum of a wrapped delay and a
		 * small lookahead wraps a SECOND time and lands back under the bound, so
		 * a Config that ought to be refused would be accepted here -- a check
		 * that passes for the wrong reason, and one whose silence would look
		 * exactly like a correct answer.
		 *
		 * A WRAPPED DELAY IS NOT THE ONLY WAY PAST 32 BITS. The count row bounds
		 * `dspCount`, but nothing bounds `hopFrames` or `lookaheadFrames` once
		 * the override is taken, so those two terms alone carry the sum over 32
		 * bits while every earlier row is satisfied and no subtraction has
		 * wrapped. Narrowing this sum accepts such a Config.
		 *
		 * WHAT THE WIDTH DOES NOT ESTABLISH: it bounds neither term. It holds
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

		/* SCH-17's rule, now reported through a status. `g_useJIT` is a
		 * `static constexpr`, so the second half folds at compile time. */
		if(_config.backend != Backend::Jit || !dsp56k::g_useJIT)
		{
			_outStatus = Status::BadBackend;
			return nullptr;
		}

		_outStatus = Status::Ok;
		return std::unique_ptr<Scheduler>(new Scheduler(_config, _executor, _board));
	}

	/* THE SCHEDULER DRIVES THE BOARD'S OWN DSP SET AND CONSTRUCTS NONE, and
	 * that is a decision with four reasons rather than a convenience.
	 *
	 *   1. The Board's set is the BRIDGED one -- board.cpp holds the only
	 *      production call of attachHdi08Bridges -- and a bridge carries the
	 *      landed flag the run gate borrows.
	 *   2. attachHdi08Bridges REFUSES a second attach, so a second set could
	 *      not be bridged after the fact.
	 *   3. The factory already receives the Board by reference, so the set
	 *      costs no new argument, no new member and no new lifetime.
	 *   4. dspSet.h answers NULL for a slot with no bridge, and SCH-33's gate
	 *      reads NULL as NOT LANDED -- so every slot of a Scheduler-owned set
	 *      would stay shut for the life of the program while every check here
	 *      stayed green.
	 *
	 * THE CHAIN CALLBACKS ARE INSTALLED HERE AND NOWHERE ELSE, because this is
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

		attachChainCallbacks(m_chain, set);

		/* `dspCount` HAS ALREADY BEEN VETTED AGAINST kJobCount by the table
		 * above, so the two bounds cannot disagree here. */
		for(unsigned i = 0; i < static_cast<unsigned>(kJobCount); ++i)
		{
			DspContext& c = m_contexts[i];

			c.base.fault            = JobFault::None;
			c.position              = i;
			c.rate                  = _config.dspRate;

			/* ZEROED RATHER THAN LEFT INDETERMINATE. SCH-12's debt loop reads
			 * the accumulator before it ever writes one. */
			c.acc                   = 0;
			c.debt                  = 0;
			c.longDispatchQuanta    = 0;

			/* BORROWED FROM THE BOARD'S SET, one slot for each position. */
			c.dsp                   = &set.dsp(i);
			c.audioEsai             = &set.peripherals(i).getEsai();
			c.secondEsai            = &set.peripherals(i).getEsai1();

			c.frameIndex            = m_frameIndex;

			/* THE RUN GATE'S POINTER, BORROWED AND NEVER COPIED. The producer
			 * sets its own flag when the download completes, and a value taken
			 * here could never see it. NULL for a slot with no bridge, which
			 * SCH-33's gate already reads as NOT LANDED. */
			c.programLanded         = set.programLanded(i);

			c.secondBusFrameDivider = _config.secondBusFrameDivider;

			m_jobs[i].fn  = &dspJob;
			m_jobs[i].ctx = &c.base;
		}
	}

	void Scheduler::mark(const TracePhase _phase, const uint64_t _frameIndex) const noexcept
	{
		if(m_trace != nullptr)
			m_trace->onPhase(_phase, _frameIndex);
	}

	/* ONE QUANTUM IS DESIGN SECTION 13.5's ORDER AND THIS IS ITS ONLY SITE.
	 *
	 *     swap     ChainAdapter::advanceAll(frameIndex)
	 *     ingress  ChainAdapter::injectCodecSource(frame)
	 *     run      0  Panel::tick(frameIndex)
	 *              1  Board::tickSofIfDue(frameIndex), then Board::runMcu(...)
	 *              2  DSP 0 .. DSP 7, ascending
	 *     egress   ChainAdapter::extractCodecSink(frame)
	 *
	 * THE PANEL AND THE MCU RUN SERIALLY HERE, OUTSIDE THE EXECUTOR, which is
	 * why the job array holds the eight DSP contexts and nothing else.
	 *
	 * NO ALLOCATION HAPPENS IN THIS FUNCTION. The adapter, the contexts and the
	 * job array are all built once, at construction. */
	void Scheduler::runFrames(const size_t _frames) noexcept
	{
		for(size_t f = 0; f < _frames; ++f)
		{
			const uint64_t frameIndex = m_frameIndex;

			/* THE FRAME INDEX REACHES EVERY CONTEXT BEFORE Executor::run, AND
			 * NO JOB WRITES IT. It is a copy and not a pointer, so a job cannot
			 * observe another job's state through it. */
			for(auto& c : m_contexts)
				c.frameIndex = frameIndex;

			mark(TracePhase::Swap, frameIndex);
			m_chain.advanceAll(frameIndex);

			mark(TracePhase::Ingress, frameIndex);
			m_chain.injectCodecSource(m_codecSource);

			mark(TracePhase::Panel, frameIndex);
			m_board.panel().tick(frameIndex);

			/* IMMEDIATELY BEFORE runMcu, design section 13.5. The Board owns
			 * the due test and this call is unconditional. */
			mark(TracePhase::Sof, frameIndex);
			m_board.tickSofIfDue(frameIndex);

			/* THE ALLOCATION ALONE. The MCU's budget/want/debt block is
			 * SCH-30's; `runMcu` already takes a cycle budget and returns what
			 * it spent, and nothing here yet carries that return. */
			mark(TracePhase::Mcu, frameIndex);
			(void) m_board.runMcu(alloc(m_mcuRate, &m_mcuAcc));

			mark(TracePhase::Dsp, frameIndex);
			m_executor.run(m_jobs, kJobCount);

			mark(TracePhase::Egress, frameIndex);
			m_chain.extractCodecSink(m_codecSink);

			++m_frameIndex;
		}
	}
}
