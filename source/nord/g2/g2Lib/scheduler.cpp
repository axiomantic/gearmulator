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
 * NO EXCEPTION ANYWHERE IN THIS FILE, AND NO ASSERTION IN `create`. Design
 * section 13.10 rule 2 forbids throwing, and a release build removes an
 * assertion, so every REJECTION has to survive NDEBUG: the return value and
 * the status out-param are the whole observable of the table below.
 *
 * THE ONE assert() IN THIS FILE IS beginPlayPhase's, AND IT IS THE SPEC'S.
 * Design section 13.10 rule 3 step 1 requires that a debug build assert both
 * codec queues were empty on entry. It is a DEBUG-ONLY CROSS-CHECK OF THE BOOT
 * REGIME and it is the predicate of nothing: every property SCH-21's check
 * reports is read back through the observable surface below, so this file
 * reports identically with and without NDEBUG. An earlier revision of this
 * comment claimed "no assertion ANYWHERE in this file", which stopped being
 * true when beginPlayPhase landed; the claim is narrowed here rather than left
 * standing and false.
 */

#include "scheduler.h"

#include "board.h"
#include "cycleDebt.h"
#include "executor.h"

#include <cassert>
#include <cstdint>
#include <thread>

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
		, m_mcu{ _config.mcuRate, 0, 0, 0 }
		, m_source(static_cast<size_t>(_config.lookaheadFrames) + _config.maxHostBlockFrames)
		, m_sink  (static_cast<size_t>(_config.lookaheadFrames) + _config.maxHostBlockFrames)
		, m_lookaheadFrames(_config.lookaheadFrames)
		, m_codecCapacity(static_cast<size_t>(_config.lookaheadFrames) + _config.maxHostBlockFrames)
		, m_underrunBase(_config.dspCount, 0)
		, m_secondUnderrunBase(_config.dspCount, 0)
		, m_phaseErrorBase(_config.dspCount, 0)
	{
		DspSet& set = _board.dspSet();

		/* `dspCount` HAS ALREADY BEEN VETTED AGAINST kJobCount by the table
		 * above, so the two bounds cannot disagree here. */
		for(unsigned i = 0; i < static_cast<unsigned>(kJobCount); ++i)
		{
			DspContext& c = m_contexts[i];

			c.base.fault            = JobFault::None;
			c.position              = i;
			c.rate                  = _config.dspRate;

			/* THE ACCUMULATOR, THE CYCLE DEBT AND THE LONG-DISPATCH COUNTER
			 * ARE ZERO, AND THE `{}` ON m_contexts IS WHAT MAKES THEM ZERO --
			 * this loop deliberately writes none of the three. SCH-12's debt
			 * loop reads the accumulator before it ever writes one, so the
			 * zero is required; writing it here as well would read like the
			 * guarantee without being it, and no mutation of such a write
			 * could go red. */

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

		/* THE DISPATCH SET STARTS FULL. Nothing has faulted, so this is the
		 * whole job array -- and building it HERE rather than special-casing
		 * "no fault yet" in runFrames means the dispatch path has exactly one
		 * shape from the first quantum onwards. */
		rebuildDispatchSet();

		/* LAST, AND THE ORDER IS LOAD-BEARING. This call publishes callbacks
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

	/* ONE QUANTUM IS DESIGN SECTION 13.5's ORDER AND THIS IS ITS ONLY SITE.
	 *
	 *     swap     ChainAdapter::advanceAll(frameIndex)
	 *     ingress  ChainAdapter::injectCodecSource(frame)      PLAY REGIME ONLY
	 *     run      0  Panel::tick(frameIndex)
	 *              1  Board::tickSofIfDue(frameIndex), then Board::runMcu(...)
	 *              2  DSP 0 .. DSP 7, ascending
	 *     egress   ChainAdapter::extractCodecSink(frame)       PLAY REGIME ONLY
	 *
	 * THE TWO PLAY-ONLY PHASES RUN IN THE PLAY REGIME AND IN NO OTHER. The
	 * object knows its regime and THE CALLER DOES NOT SELECT IT: a Scheduler is
	 * born in Boot and beginPlayPhase is the one thing that moves it to Play.
	 * A boot quantum's trace is therefore five records -- Swap, Panel, Sof,
	 * Mcu, Dsp -- and a play quantum's is seven, with the INGRESS RECORD BEFORE
	 * THE WHOLE RUN PHASE and the EGRESS RECORD AFTER IT, which is the order
	 * the diagram above states.
	 *
	 * WITHOUT THE BOOT REGIME THE SINK FILLS after L + B boot quanta and the
	 * scheduler stops part-way through the boot. That is the defect the regime
	 * closes, and it is a real one rather than a precaution: CodecSink::push
	 * REFUSES when full and this loop stops when it does.
	 *
	 * THE PANEL AND THE MCU RUN SERIALLY HERE, OUTSIDE THE EXECUTOR, which is
	 * why the job array holds the eight DSP contexts and nothing else.
	 *
	 * NO ALLOCATION HAPPENS IN THIS FUNCTION. The adapter, the contexts, the
	 * job array and both codec queues are all built once, at construction, and
	 * the one Frame the two codec phases need is a local. */
	void Scheduler::runFrames(const size_t _frames) noexcept
	{
		/* THE OWNING THREAD IS RECORDED ON THE FIRST CALL AFTER EACH CLEARING,
		 * and beginPlayPhase's step 5 is what clears it -- so the first
		 * runFrames of the play phase re-establishes it on the audio thread
		 * instead of tripping on the boot thread's identity. */
		if(m_owner == std::thread::id{})
		{
			m_owner = std::this_thread::get_id();
		}
		else
		{
			/* THE DEBUG-ONLY CROSS-CHECK OF THE THREAD MAP, and it is the
			 * PREDICATE OF NOTHING. docs/threading.md gives runFrames to one
			 * thread at a time and design section 13.10.5 has ownership move
			 * exactly once, at beginPlayPhase; a second thread calling in is a
			 * data race on every member below, and this is where it is loudest.
			 *
			 * A RELEASE BUILD REMOVES IT, WHICH IS WHY owningThread() EXISTS.
			 * The property that stops the race in the SHIPPED build has to be
			 * checkable in the shipped build, so t0_thread_map reads the
			 * recorded identity back through the accessor and never through
			 * this line. */
			assert(m_owner == std::this_thread::get_id()
				&& "runFrames was called from a thread that does not own this Scheduler");
		}

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

			/* THE INGRESS, AND IT PRECEDES THE WHOLE RUN PHASE. front()
			 * answers a ZERO frame when the queue is empty, never stale data,
			 * and pop() is where a starve is counted -- exactly once for each
			 * quantum that consumed one. */
			if(m_regime == CodecRegime::Play)
			{
				mark(TracePhase::Ingress, frameIndex);
				m_chain.injectCodecSource(m_source.front());
				m_source.pop();
			}

			mark(TracePhase::Panel, frameIndex);
			m_board.panel().tick(frameIndex);

			/* IMMEDIATELY BEFORE runMcu, design section 13.5. The Board owns
			 * the due test and this call is unconditional. */
			mark(TracePhase::Sof, frameIndex);
			m_board.tickSofIfDue(frameIndex);

			/* THE SAME BLOCK OF DESIGN SECTION 13.4.6 THAT THE DSP JOB USES,
			 * INSTANTIATED AGAINST THE MCU CONTEXT. It is one block used
			 * twice and not two blocks that resemble each other: the budget,
			 * the want, the `want <= 0` branch and the floor at zero are all
			 * g2::runQuantum's, and the only thing this site supplies is the
			 * role-filler. Design section 13.4.6 states that the MCU context
			 * has no receive/transmit bracket and that this block is the whole
			 * of its quantum, which is why there is nothing around this call.
			 *
			 * THE RETURN IS A MEASUREMENT AND NOT STATE. What decides the next
			 * quantum is carried in m_mcu.debt and m_mcu.acc, which the block
			 * mutates. */
			mark(TracePhase::Mcu, frameIndex);
			(void) runQuantum(m_mcu, [this](const uint32_t _want) noexcept
			{
				return m_board.runMcu(_want);
			});

			mark(TracePhase::Dsp, frameIndex);
			m_executor.run(m_liveJobs, m_liveCount);

			/* THE FAULT IS READ AFTER run() RETURNS, design section 13.10.5,
			 * and it covers the MCU's bit as well as the eight job contexts --
			 * the MCU ran earlier in this same quantum. A NEW fault takes the
			 * faulted context out of the dispatch set from the NEXT quantum
			 * onwards; the quantum that produced it already ran. */
			if(latchFaults())
				rebuildDispatchSet();

			/* THE EGRESS, AND IT FOLLOWS THE WHOLE RUN PHASE. A refused push
			 * is a DEFECT REPORT and not a condition to handle: the capacity
			 * L + B makes it unreachable in a correct build, CodecSink counts
			 * it in droppedFrames, and this loop STOPS rather than overwriting
			 * a frame the host has already been told to expect. The quantum
			 * itself ran to completion, so the frame index advances first. */
			if(m_regime == CodecRegime::Play)
			{
				mark(TracePhase::Egress, frameIndex);

				Frame out{};
				m_chain.extractCodecSink(out);

				if(!m_sink.push(out))
				{
					++m_frameIndex;
					return;
				}
			}

			++m_frameIndex;
		}
	}

	/* THE BOOT-TO-PLAY TRANSITION. Design section 13.10 rule 3's five steps,
	 * in its order, and the order is load-bearing at step 3.
	 *
	 * WHAT IT DOES NOT TOUCH: emulated machine state, any fault -- which is
	 * sticky and which only reset() clears -- and the virtual frame index,
	 * which its own priming quanta advance by exactly L. */
	void Scheduler::beginPlayPhase() noexcept
	{
		/* STEP 1. Whatever the boot left is discarded. IN A CORRECT BUILD BOTH
		 * ARE ALREADY EMPTY, because the boot regime kept the boot out of
		 * them, and this assertion is what makes that a checkable property
		 * rather than an assumption. It is the predicate of no check: SCH-21's
		 * check reads the hand-off state back through the observable surface,
		 * so it reports identically under NDEBUG. */
		assert(m_source.size() == 0 && "the boot regime left the CodecSource untouched");
		assert(m_sink.size()   == 0 && "the boot regime left the CodecSink untouched");

		/* RECONSTRUCTION IS WHAT CLEARS THEM, and it clears the four counters
		 * they own at the same time -- which is half of step 3. SCH-15's two
		 * queues expose neither a clear nor a counter reset, and each ring is
		 * already the right size, so neither assignment reallocates. This runs
		 * on the boot thread and never inside a quantum. */
		m_source = CodecSource(m_codecCapacity);
		m_sink   = CodecSink  (m_codecCapacity);

		/* STEP 2. Exactly `lookaheadFrames` zero frames. The push cannot
		 * refuse: the capacity is L + B and the queue was just emptied. */
		const Frame zero{};

		for(unsigned i = 0; i < m_lookaheadFrames; ++i)
			(void) m_source.push(zero);

		/* STEP 3, AND IT HAPPENS BEFORE THE PRIMING RUN OF STEP 4 rather than
		 * after it, so the priming run's OWN counters stay visible. A DSP that
		 * underruns during priming is a real finding and clearing afterwards
		 * would hide it.
		 *
		 * THE THREE ADAPTER-OWNED COUNTERS ARE ZEROED BY MOVING THIS OBJECT'S
		 * BASELINE, not by touching the ChainAdapter: its mailboxes are
		 * emulated state and this call may not touch that. scheduler.h's
		 * accessor comment carries the whole reasoning. */
		for(unsigned p = 0; p < static_cast<unsigned>(m_underrunBase.size()); ++p)
		{
			m_underrunBase[p]       = m_chain.underrunFrames(p);
			m_secondUnderrunBase[p] = m_chain.secondBusUnderrunFrames(p);
			m_phaseErrorBase[p]     = m_chain.phaseErrorFrames(p);
		}

		/* THE TWO DIAGNOSTIC COUNTERS, AT EVERY CONTEXT INDEX. The rational
		 * accumulators are NOT reset: they are emulated timebase state, not
		 * observability. */
		m_mcu.debt               = 0;
		m_mcu.longDispatchQuanta = 0;

		for(auto& c : m_contexts)
		{
			c.debt               = 0;
			c.longDispatchQuanta = 0;
		}

		/* STEP 4. Exactly `lookaheadFrames` quanta, in the PLAY regime. They
		 * consume the L primed zero frames, so starvedFrames stays zero, and
		 * they produce L real frames into the CodecSink -- which is where
		 * design section 13.6 needs the lookahead to sit. The push-only
		 * priming an earlier draft used leaves the sink empty and the
		 * scheduler ahead of nothing. */
		m_regime = CodecRegime::Play;

		runFrames(m_lookaheadFrames);

		/* STEP 5. The first runFrames of the play phase re-establishes the
		 * owner on the audio thread. Without this the owner stays the boot
		 * thread's and the first real audio callback of every session trips
		 * on it. */
		m_owner = std::thread::id{};
	}

	size_t Scheduler::push(const Frame* const _in, const size_t _frames) noexcept
	{
		if(_in == nullptr)
			return 0;

		size_t accepted = 0;

		/* STOPS AT THE FIRST REFUSAL rather than skipping past it, so the
		 * frames that were accepted are a PREFIX of the block and the caller's
		 * shortfall names a contiguous tail. */
		while(accepted < _frames && m_source.push(_in[accepted]))
			++accepted;

		return accepted;
	}

	size_t Scheduler::pull(Frame* const _out, const size_t _frames) noexcept
	{
		if(_out == nullptr)
			return 0;

		return m_sink.pull(_out, _frames);
	}

	uint64_t Scheduler::frameIndex() const noexcept
	{
		return m_frameIndex;
	}

	uint64_t Scheduler::underrunFrames(const unsigned _position) const noexcept
	{
		if(_position >= m_underrunBase.size())
			return 0;

		return m_chain.underrunFrames(_position) - m_underrunBase[_position];
	}

	uint64_t Scheduler::secondBusUnderrunFrames(const unsigned _position) const noexcept
	{
		if(_position >= m_secondUnderrunBase.size())
			return 0;

		return m_chain.secondBusUnderrunFrames(_position) - m_secondUnderrunBase[_position];
	}

	uint64_t Scheduler::phaseErrorFrames(const unsigned _position) const noexcept
	{
		if(_position >= m_phaseErrorBase.size())
			return 0;

		return m_chain.phaseErrorFrames(_position) - m_phaseErrorBase[_position];
	}

	uint64_t Scheduler::starvedFrames()   const noexcept { return m_source.starvedFrames();  }
	uint64_t Scheduler::overflowFrames()  const noexcept { return m_source.overflowFrames(); }
	uint64_t Scheduler::droppedFrames()   const noexcept { return m_sink.droppedFrames();    }
	uint64_t Scheduler::underflowFrames() const noexcept { return m_sink.underflowFrames();  }

	/* INDEX 0 IS THE MCU AND 1 .. dspCount ARE THE DSPs, design section 13.5's
	 * CONTEXT INDEX column and not its ORDER column. The two differ, and an
	 * implementation that read the order column would answer for the panel at
	 * index 0 and be off by one everywhere above it. */
	int64_t Scheduler::cycleDebt(const unsigned _contextIndex) const noexcept
	{
		if(_contextIndex == 0)
			return m_mcu.debt;

		if(_contextIndex > kJobCount)
			return 0;

		return m_contexts[_contextIndex - 1].debt;
	}

	uint64_t Scheduler::longDispatchQuanta(const unsigned _contextIndex) const noexcept
	{
		if(_contextIndex == 0)
			return m_mcu.longDispatchQuanta;

		if(_contextIndex > kJobCount)
			return 0;

		return m_contexts[_contextIndex - 1].longDispatchQuanta;
	}

	/* THE FAULT SURFACE. Design section 13.10.5, and the SAME context index
	 * cycleDebt and longDispatchQuanta take: 0 is the MCU, 1 .. dspCount are
	 * the DSPs. An index above dspCount reads back the no-fault answer rather
	 * than running off the end of the latch. */
	bool Scheduler::faulted() const noexcept
	{
		return m_faulted;
	}

	bool Scheduler::contextFaulted(const unsigned _contextIndex) const noexcept
	{
		return contextFault(_contextIndex) != JobFault::None;
	}

	JobFault Scheduler::contextFault(const unsigned _contextIndex) const noexcept
	{
		if(_contextIndex > kJobCount)
			return JobFault::None;

		return m_fault[_contextIndex];
	}

	/* THE RESET. Design section 13.10.5, and the boot thread's call.
	 *
	 * THE ORDER IS LOAD-BEARING IN ONE PLACE AND NOWHERE ELSE: the three
	 * adapter-owned counter baselines are taken AFTER m_chain.reset(), because
	 * a baseline read before the adapter was zeroed would record the old
	 * reading and every observability accessor would then answer a negative
	 * difference through an unsigned type.
	 *
	 * THE RATIONAL ACCUMULATORS ARE ZEROED HERE AND beginPlayPhase DOES NOT
	 * ZERO THEM, and the two are right for the same reason rather than
	 * inconsistent: an accumulator is emulated timebase state, which a
	 * boot-to-play hand-off may not touch and which a reset of the whole
	 * machine must. */
	void Scheduler::reset() noexcept
	{
		for(auto& f : m_fault)
			f = JobFault::None;

		m_faulted = false;

		for(auto& c : m_contexts)
		{
			c.base.fault         = JobFault::None;
			c.acc                = 0;
			c.debt               = 0;
			c.longDispatchQuanta = 0;
		}

		rebuildDispatchSet();

		m_mcu.acc                = 0;
		m_mcu.debt               = 0;
		m_mcu.longDispatchQuanta = 0;

		m_frameIndex = 0;
		m_regime     = CodecRegime::Boot;

		/* Reconstruction is what clears a queue and its counters. Each ring is
		 * already the right size, so neither assignment reallocates. */
		m_source = CodecSource(m_codecCapacity);
		m_sink   = CodecSink  (m_codecCapacity);

		m_board.reset();
		m_chain.reset();

		for(unsigned p = 0; p < static_cast<unsigned>(m_underrunBase.size()); ++p)
		{
			m_underrunBase[p]       = m_chain.underrunFrames(p);
			m_secondUnderrunBase[p] = m_chain.secondBusUnderrunFrames(p);
			m_phaseErrorBase[p]     = m_chain.phaseErrorFrames(p);
		}

		m_owner = std::thread::id{};
	}

	void Scheduler::rebuildDispatchSet() noexcept
	{
		m_liveCount = 0;

		for(size_t i = 0; i < kJobCount; ++i)
		{
			if(m_fault[i + 1u] != JobFault::None)
				continue;

			m_liveJobs[m_liveCount] = m_jobs[i];
			++m_liveCount;
		}
	}

	/* THE FAULT IS READ BACK AFTER THE PHASE THAT COULD HAVE PRODUCED IT, and
	 * the first latch of an index WINS. A context that faulted is not
	 * dispatched again, so its `base.fault` cannot be overwritten -- but the
	 * MCU's bit can be, because Board::runMcu rewrites it on every call, and a
	 * later clear must not silently retract a fault the Device may already have
	 * acted on. Design section 13.10.5 calls the fault sticky and this is where
	 * that word is implemented. */
	bool Scheduler::latchFaults() noexcept
	{
		bool latched = false;

		if(m_fault[0] == JobFault::None && m_board.faulted())
		{
			m_fault[0] = JobFault::CoreHalted;
			m_faulted  = true;
		}

		for(size_t i = 0; i < kJobCount; ++i)
		{
			if(m_fault[i + 1u] != JobFault::None)
				continue;

			const JobFault f = m_contexts[i].base.fault;

			if(f == JobFault::None)
				continue;

			m_fault[i + 1u] = f;
			m_faulted       = true;
			latched         = true;
		}

		return latched;
	}

	std::thread::id Scheduler::owningThread() const noexcept
	{
		return m_owner;
	}
}
