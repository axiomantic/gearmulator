/* t0_order.cpp -- the order inside one quantum.
 *
 * What one quantum is, and what this file observes of it. Scheduler::runFrames
 * turns whole quanta, in this order:
 *
 *     swap     ChainAdapter::advanceAll(frameIndex)
 *     ingress  ChainAdapter::injectCodecSource(frame)      play regime only
 *     run      0  Panel::tick(frameIndex)
 *              1  Board::tickSofIfDue(frameIndex), then Board::runMcu(...)
 *              2  DSP 0 .. DSP 7, ascending
 *     egress   ChainAdapter::extractCodecSink(frame)       play regime only
 *
 * The two play-only phases are not asserted here and that is not an omission.
 * A Scheduler built here carries no regime member, so it is the boot machine
 * by construction and its quantum is FIVE records -- Swap, Panel, Sof, Mcu,
 * Dsp -- each carrying that quantum's frame index. Nothing in this file says
 * whether either play-only phase ever runs.
 *
 * The observation seams are three and the check opens no other. The injected
 * Executor is handed the job array, so every DspContext is legally reachable
 * through Job::ctx -- dspContext.h's two static_asserts sanction exactly that
 * recovery. Board::dspSet() is a PUBLIC accessor, so the cores and the ESAI
 * ports the contexts are supposed to borrow are reachable for an ADDRESS
 * comparison. Neither reaches the phases that run serially in the Scheduler,
 * so Config::trace carries those.
 *
 * Why the trace carries a separate tag for the start-of-frame tick.
 * `tickSofIfDue` runs IMMEDIATELY BEFORE `runMcu`. Board is
 * final with no virtual member, so no test double substitutes for it and the
 * adjacency has no other decider; folding the two into one tag would leave an
 * ordered imperative with nothing able to report it.
 *
 * What this file does not establish, said plainly rather than left to a reader.
 * It does not establish that a DSP ran. Every slot's run gate is shut here --
 * no firmware is downloaded, so every bridge's landed flag is false and
 * dspJob's step 2 is skipped for all eight -- and the trace records DISPATCH
 * and not EXECUTION. The wiring case below is what a shut gate still
 * discriminates: the pointer the gate reads is non-null and is the BRIDGE's
 * own flag, by address. A per-slot cycle counter that advances is the honest
 * observable for execution and it needs an open gate, which needs a completed
 * firmware download.
 *
 * No case here is a language assert() and no case catches an exception, so
 * this file reports identically under NDEBUG and without it.
 */

#include "board.h"
#include "chainAdapter.h"
#include "dspContext.h"
#include "esaiFrame.h"
#include "executor.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/peripherals56311.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{

	/* The position-to-port order every Scheduler in this file is built with.
	 *
	 * On a real machine the order is the firmware's and it is not the identity;
	 * a Scheduler built with an empty Config::chainOrder therefore leaves its
	 * ports idle and wires the chain only once the firmware's own table can be
	 * read. No firmware runs in this file, so such a Scheduler would never wire
	 * its chain at all and every arrival this file asserts would be an arrival
	 * of nothing.
	 *
	 * So the order is supplied, and the identity is the right one here: this
	 * file drives the ESAIs itself and holds every position to the slot of the
	 * same number. It is a harness's choice and never a claim about the machine.
	 */
	std::vector<unsigned> identityChainOrder()
	{
		std::vector<unsigned> order(g2::kJobCount);

		for(unsigned i = 0; i < static_cast<unsigned>(g2::kJobCount); ++i)
			order[i] = i;

		return order;
	}
	int g_failures = 0;
	int g_cases    = 0;

	void check(const bool _condition, const char* const _what)
	{
		++g_cases;

		if(_condition)
			return;

		std::printf("FAIL %s\n", _what);
		++g_failures;
	}

	void checkEqual(const uint64_t _observed, const uint64_t _expected, const char* const _what)
	{
		++g_cases;

		if(_observed == _expected)
			return;

		std::printf("FAIL %s: observed %llu, expected %llu\n", _what,
			static_cast<unsigned long long>(_observed), static_cast<unsigned long long>(_expected));
		++g_failures;
	}

	void checkSame(const void* const _observed, const void* const _expected, const char* const _what)
	{
		++g_cases;

		if(_observed == _expected)
			return;

		std::printf("FAIL %s: observed %p, expected %p\n", _what, _observed, _expected);
		++g_failures;
	}

	const char* phaseName(const g2::TracePhase _phase)
	{
		switch(_phase)
		{
		case g2::TracePhase::Swap:    return "swap";
		case g2::TracePhase::Ingress: return "ingress";
		case g2::TracePhase::Panel:   return "panel";
		case g2::TracePhase::Sof:     return "sof";
		case g2::TracePhase::Mcu:     return "mcu";
		case g2::TracePhase::Dsp:     return "dsp";
		case g2::TracePhase::Egress:  return "egress";
		}
		return "?";
	}

	/* The trace sink. It records the phase tag and the frame index of every
	 * phase, in the order the Scheduler emits them, and it grows no storage
	 * inside a noexcept callback. */
	class RecordingTrace final : public g2::TraceSink
	{
	public:
		static constexpr size_t kMax = 256;

		void onPhase(const g2::TracePhase _phase, const uint64_t _frameIndex) noexcept override
		{
			if(m_count < kMax)
			{
				m_phase[m_count] = _phase;
				m_frame[m_count] = _frameIndex;
			}
			++m_count;
		}

		size_t         count()             const { return m_count; }
		g2::TracePhase phase(const size_t i) const { return m_phase[i]; }
		uint64_t       frame(const size_t i) const { return m_frame[i]; }
		void           clear()                   { m_count = 0; }

	private:
		size_t         m_count = 0;
		g2::TracePhase m_phase[kMax]{};
		uint64_t       m_frame[kMax]{};
	};

	/* The injected executor. It RUNS every job -- an Executor that did not
	 * would not be one -- and it records, BEFORE each job body runs, the whole
	 * of the context that body is about to see. Reading the members after the
	 * body would confuse what the Scheduler wired with what the job did. */
	class RecordingExecutor final : public g2::Executor
	{
	public:
		static constexpr size_t kMaxRuns = 32;

		struct Record
		{
			size_t             count = 0;
			const g2::DspContext* ctx[g2::kJobCount]{};
			unsigned           position[g2::kJobCount]{};
			uint64_t           frameIndex[g2::kJobCount]{};
			uint32_t           acc[g2::kJobCount]{};
			int64_t            debt[g2::kJobCount]{};
			uint64_t           longDispatchQuanta[g2::kJobCount]{};
			g2::JobFault       fault[g2::kJobCount]{};
		};

		void run(const Job* const _jobs, const size_t _count) noexcept override
		{
			if(m_runs < kMaxRuns)
			{
				Record& r = m_record[m_runs];
				r.count = _count;

				for(size_t i = 0; i < _count && i < g2::kJobCount; ++i)
				{
					const auto* const c = reinterpret_cast<const g2::DspContext*>(_jobs[i].ctx);

					r.ctx[i]                = c;
					r.position[i]           = c->position;
					r.frameIndex[i]         = c->frameIndex;
					r.acc[i]                = c->acc;
					r.debt[i]               = c->debt;
					r.longDispatchQuanta[i] = c->longDispatchQuanta;
					r.fault[i]              = c->base.fault;
				}
			}

			++m_runs;

			for(size_t i = 0; i < _count; ++i)
				_jobs[i].fn(_jobs[i].ctx);
		}

		bool isSerial() const noexcept override { return true; }

		size_t        runs()               const { return m_runs; }
		const Record& record(const size_t i) const { return m_record[i]; }

	private:
		size_t m_runs = 0;
		Record m_record[kMaxRuns]{};
	};

	/* The clock control registers are programmed for a one-slot frame, which
	 * is what lets readRX(0) report slot 0. Copied from t0_chain_data_flow,
	 * which drives the same peripheral for the same reason. */
	void enableTransmitter(dsp56k::Esai& _esai)
	{
		_esai.writeTransmitClockControlRegister(0);
		_esai.writeTransmitControlRegister(1u << dsp56k::Esai::M_TE0);
	}

	void enableReceiver(dsp56k::Esai& _esai)
	{
		_esai.writeReceiveClockControlRegister(0);
		_esai.writeReceiveControlRegister(1u << dsp56k::Esai::M_RE0);
	}

	/* The second bus transmits on TX2 and not on TX0 (frame.h's register
	 * table), so its transmitter has to be enabled as well as TX0's -- an
	 * enabled-but-unwritten transmitter underruns on every frame, and the
	 * written-flag rule reads the latch that records it.
	 *
	 * M_TUE is a slot-lifetime status and is not usable here: writeSlotToFrame
	 * raises it and then triggers the transmit DMA, whose service reaches
	 * Esai::writeTX and clears it again before the frame is delivered.
	 * Esai::txUnderrunInFrame() lives as long as the frame it describes. */
	void enableSecondTransmitter(dsp56k::Esai& _esai)
	{
		_esai.writeTransmitClockControlRegister(0);
		_esai.writeTransmitControlRegister((1u << dsp56k::Esai::M_TE0) | (1u << dsp56k::Esai::M_TE2));
	}

	/* The five unconditional phases of one quantum, in order. The ingress and
	 * the egress are NOT here: both are play regime only, a Scheduler built
	 * here carries no regime member and is the
	 * boot machine by construction, and the play regime is what adds the
	 * regime, the two calls and the codec queues that feed them. A sequence
	 * naming either would assert an observable this file cannot produce. */
	constexpr g2::TracePhase kQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp
	};

	constexpr size_t kPhasesPerQuantum = sizeof(kQuantum) / sizeof(kQuantum[0]);

	/* ---------------------------------------------------------------------
	 * CASE 1. The order of one quantum, and of two consecutive ones.
	 *
	 * The whole sequence is asserted position by position against the table
	 * above, which is what makes "a trace that omits the swap fails" a
	 * property of this check rather than a sentence about it: an omission
	 * shifts every later tag and the first comparison reports it.
	 */
	void caseOrder(RecordingTrace& _trace, RecordingExecutor& _executor, g2::Scheduler& _scheduler)
	{
		_scheduler.runFrames(2);

		checkEqual(_trace.count(), 2u * kPhasesPerQuantum,
			"two quanta emit exactly two quanta of phases");

		if(_trace.count() != 2u * kPhasesPerQuantum)
			return;

		for(size_t q = 0; q < 2; ++q)
		{
			for(size_t p = 0; p < kPhasesPerQuantum; ++p)
			{
				const size_t at = q * kPhasesPerQuantum + p;

				char what[256];

				std::snprintf(what, sizeof(what), "quantum %zu phase %zu is %s (observed %s)",
					q, p, phaseName(kQuantum[p]), phaseName(_trace.phase(at)));
				check(_trace.phase(at) == kQuantum[p], what);

				std::snprintf(what, sizeof(what), "quantum %zu phase %s carries frame index %zu",
					q, phaseName(kQuantum[p]), q);
				checkEqual(_trace.frame(at), q, what);
			}
		}

		checkEqual(_executor.runs(), 2u, "two quanta dispatch the job array exactly twice");
	}

	/* ---------------------------------------------------------------------
	 * CASE 2. The eight DSP bodies are dispatched ascending, and every context
	 * is wired to the Board's own set BY ADDRESS.
	 *
	 * The address comparison is the one that discriminates the rescope. A
	 * Scheduler holding a DSP set of its own would satisfy every other
	 * assertion in this file and fail exactly these three, because the two
	 * sets' cores and ports sit at different addresses.
	 */
	void caseContexts(const RecordingExecutor& _executor, g2::Board& _board, const g2::Scheduler::Config& _config)
	{
		if(_executor.runs() < 2)
		{
			check(false, "the job array was dispatched at least twice");
			return;
		}

		g2::DspSet& set = _board.dspSet();

		for(size_t q = 0; q < 2; ++q)
		{
			const RecordingExecutor::Record& r = _executor.record(q);

			checkEqual(r.count, g2::kJobCount, "the job array holds exactly kJobCount jobs");

			if(r.count != g2::kJobCount)
				continue;

			for(unsigned i = 0; i < g2::kJobCount; ++i)
			{
				char what[256];

				std::snprintf(what, sizeof(what), "quantum %zu job %u is chain position %u (observed %u)",
					q, i, i, r.position[i]);
				checkEqual(r.position[i], i, what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u carries this quantum's frame index", q, i);
				checkEqual(r.frameIndex[i], q, what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u borrows the BOARD's core for that slot", q, i);
				checkSame(r.ctx[i]->dsp, &set.dsp(i), what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u borrows the BOARD's audio ESAI for that slot", q, i);
				checkSame(r.ctx[i]->audioEsai, &set.peripherals(i).getEsai(), what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u borrows the BOARD's second-bus ESAI for that slot", q, i);
				checkSame(r.ctx[i]->secondEsai, &set.peripherals(i).getEsai1(), what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u carries the Config's second-bus divider", q, i);
				checkEqual(r.ctx[i]->secondBusFrameDivider, _config.secondBusFrameDivider, what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u carries the Config's DSP rate numerator", q, i);
				checkEqual(r.ctx[i]->rate.num, _config.dspRate.num, what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u carries the Config's DSP rate denominator", q, i);
				checkEqual(r.ctx[i]->rate.den, _config.dspRate.den, what);

				/* The run gate's own wiring, and it is both halves. Non-null
				 * alone would pass against a pointer at anything at all; the
				 * address is what ties it to the bridge whose download
				 * completes. Omit the write and all eight read null. */
				std::snprintf(what, sizeof(what),
					"quantum %zu context %u carries a NON-NULL landed-flag pointer", q, i);
				check(r.ctx[i]->programLanded != nullptr, what);

				std::snprintf(what, sizeof(what),
					"quantum %zu context %u borrows the BOARD's landed flag for that slot", q, i);
				checkSame(r.ctx[i]->programLanded, set.programLanded(i), what);

				std::snprintf(what, sizeof(what), "quantum %zu context %u reports no fault", q, i);
				checkEqual(static_cast<uint64_t>(r.fault[i]),
					static_cast<uint64_t>(g2::JobFault::None), what);
			}
		}

		/* The three zeroed members, read at the first dispatch -- before any
		 * job body has run. The debt loop reads the accumulator before it
		 * ever writes one, so an indeterminate value here is a defect no later
		 * reading could separate from a legitimate one.
		 *
		 * These three are traced and not demonstrated, and saying so is the
		 * point. Nothing this file can mutate reaches them. They are read at
		 * the FIRST dispatch, before any job body has run, so no write by
		 * dspJob can reach them whatever it does -- and dspJob writes these
		 * three only through runQuantum, inside its step 2, behind the run
		 * gate, which is shut for all eight slots in every case in this file
		 * (MEASURED with a probe inside the gate: it opens zero times in a
		 * whole run). The constructor deliberately writes none of the three
		 * either, so the value initialiser on Scheduler's context array is the
		 * only mechanism that puts a zero there and no mutation of the
		 * constructor can make them fail.
		 *
		 * Deleting that value initialiser also leaves them green, and not for
		 * the reason a reader would guess. It is not that the Scheduler lands
		 * on fresh pages: MEASURED, with a block of exactly sizeof(Scheduler)
		 * filled with 0xA5 and freed immediately before, the Scheduler lands on
		 * That same address and the three still read zero on entry to the
		 * constructor. This platform's allocator zeroes the block, which is a
		 * property of this platform and not a guarantee of anything.
		 *
		 * What they still catch is a constructor that writes a NON-zero value
		 * into one of the three. */
		const RecordingExecutor::Record& first = _executor.record(0);

		for(unsigned i = 0; i < g2::kJobCount && first.count == g2::kJobCount; ++i)
		{
			char what[256];

			std::snprintf(what, sizeof(what), "context %u enters its first quantum with a zero accumulator", i);
			checkEqual(first.acc[i], 0u, what);

			std::snprintf(what, sizeof(what), "context %u enters its first quantum with zero cycle debt", i);
			checkEqual(static_cast<uint64_t>(first.debt[i]), 0u, what);

			std::snprintf(what, sizeof(what), "context %u enters its first quantum with a zero long-dispatch counter", i);
			checkEqual(first.longDispatchQuanta[i], 0u, what);
		}
	}

	/* ---------------------------------------------------------------------
	 * CASE 3. A frame crosses the chain, which is the only assertion in this
	 * file that reports whether the chain-callback installer ran.
	 *
	 * Why not the written flag or the underrun counter. Every run gate is shut
	 * here, so no DSP writes a transmit register on its own and every
	 * position's written flag is false in a correct build and in an
	 * uninstalled one alike; the underrun counter rises once per position per
	 * quantum in both worlds for the same reason. The crossing separates them,
	 * because the installer is the ONLY thing that binds a position's ESAI to
	 * the adapter's mailboxes.
	 *
	 * Why it also reports the adapter'S dspCount. The audio bus is a Line of
	 * dspCount + 1 mailboxes; a wrong count truncates the array and the higher
	 * positions stop crossing, so driving EVERY adjacent pair is what makes
	 * that argument's forwarding observable rather than assumed.
	 *
	 * The quantum count is derived from the Config and is not a literal. A
	 * mailbox is a ring of hopFrames + 1 frames and one quantum performs
	 * exactly one swap, so the count IS the hop. G2_CHAIN_HOP_FRAMES is marked
	 * PROVISIONAL at g2/timebase.h, so a literal agrees with the derivation at
	 * today's value and disagrees at tomorrow's -- MEASURED: with the constant
	 * moved to 2 and the count still written as the literal 1, every crossing
	 * below reports a zero arrival. The check cannot reach the adapter, so it
	 * derives from the Config it supplied, which is the same number.
	 * t0_chain_data_flow derives the same step count from adapter.hopFrames()
	 * for the same reason.
	 *
	 * The limit of that derivation. It keeps this case honest when the
	 * provisional constant moves; it establishes nothing about the hop the
	 * ADAPTER was handed, because this Config's hop IS the constant. Case 4 is
	 * where that forwarding is separated, at a hop the constant does not carry.
	 */
	void caseChainCrossing(g2::Board& _board, g2::Scheduler& _scheduler, const g2::Scheduler::Config& _config)
	{
		g2::DspSet& set = _board.dspSet();

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			enableTransmitter(set.peripherals(i).getEsai());
			enableReceiver(set.peripherals(i).getEsai());
		}

		/* One distinct sample for each source position, primed into the
		 * mailbox by the check's own transmit frame, then the transmit
		 * register CLEARED so that the quantum's own transmit for that
		 * position carries a zero and cannot be mistaken for the arrival. */
		for(unsigned i = 0; i + 1u < g2::kJobCount; ++i)
		{
			dsp56k::Esai& source = set.peripherals(i).getEsai();

			const dsp56k::TWord sample = 0x200000u + i + 1u;

			check(sample != 0u,
				"the injected sample is non-zero (a zero compares equal against a "
				"default-zero read whether it crossed or not)");

			source.writeTX(0u, sample);
			checkEqual(g2::transmitDspFrame(source), source.getTxWordCount() + 1u,
				"the priming transmit frame ran");
			source.writeTX(0u, 0u);
		}

		_scheduler.runFrames(_config.hopFrames);

		for(unsigned i = 0; i + 1u < g2::kJobCount; ++i)
		{
			dsp56k::Esai& sink = set.peripherals(i + 1u).getEsai();

			const dsp56k::TWord sample = 0x200000u + i + 1u;

			char what[256];
			std::snprintf(what, sizeof(what),
				"the frame injected at position %u's audio ESAI crossed to position %u's in "
				"hopFrames (%u) quanta", i, i + 1u, _config.hopFrames);
			checkEqual(sink.readRX(0u), sample, what);
		}
	}

	/* ---------------------------------------------------------------------
	 * CASE 4. The hop depth reaches the adapter'S MAILBOXES.
	 *
	 * A mailbox is a ring of hopFrames + 1 frames, so a frame written at the
	 * head first reaches the read cell after exactly hopFrames swaps. At a hop
	 * of 2 the sample is therefore ABSENT after one quantum and PRESENT after
	 * two, and both halves are asserted: the absence is what a Scheduler that
	 * handed the adapter the build constant instead of the Config's value
	 * would fail.
	 */
	void caseHopForwarded(g2::Board& _board, g2::Executor& _executor)
	{
		g2::Scheduler::Config config;
		config.chainOrder = identityChainOrder();
		config.hopFrames    = 2;
		config.testOverride = true;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, _executor, _board, status);

		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
			"a hop of 2 with the override taken is accepted");

		if(!scheduler)
		{
			check(false, "a hop of 2 with the override taken yields a Scheduler");
			return;
		}

		g2::DspSet& set = _board.dspSet();

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			enableTransmitter(set.peripherals(i).getEsai());
			enableReceiver(set.peripherals(i).getEsai());
		}

		const unsigned      source = 3;
		const dsp56k::TWord sample = 0x3A0000u;

		dsp56k::Esai& from = set.peripherals(source).getEsai();
		dsp56k::Esai& to   = set.peripherals(source + 1u).getEsai();

		/* The absent reading below cannot be a leftover from case 3. This
		 * Scheduler owns a NEW ChainAdapter whose mailboxes are all zero, and
		 * the sink's own receive frame runs inside the first quantum and
		 * latches what that adapter holds. */
		from.writeTX(0u, sample);
		checkEqual(g2::transmitDspFrame(from), from.getTxWordCount() + 1u,
			"the priming transmit frame ran at a hop of 2");
		from.writeTX(0u, 0u);

		scheduler->runFrames(1);
		checkEqual(to.readRX(0u), 0u,
			"at a hop of 2 the frame has NOT crossed after one quantum");

		scheduler->runFrames(1);
		checkEqual(to.readRX(0u), sample,
			"at a hop of 2 the frame HAS crossed after two quanta");
	}

	/* ---------------------------------------------------------------------
	 * CASE 5. The second bus's topology and its frame divider reach the
	 * adapter, which are the two of the four chain arguments the audio bus
	 * cannot report: the audio chain is fixed to a Line at every topology, and
	 * it advances on every quantum whatever the divider says.
	 *
	 * THE DIVIDER. A frame primed into a second-bus mailbox may not move on a
	 * NON-window quantum and must have moved by the next window one. Both
	 * halves are asserted, and the first is the discriminating one: an adapter
	 * built with a divider of 1 advances the second bus every quantum and
	 * delivers the frame early.
	 *
	 * THE TOPOLOGY. On a Ring the tail position writes the head's mailbox, so
	 * position 7's frame arrives at position 0. On a Line it would write a
	 * ninth mailbox that nothing reads, and position 0 would read a mailbox
	 * nothing writes. The wrap is therefore the assertion that separates the
	 * two, and it is why every position is driven rather than one.
	 *
	 * WHY THE "NOT YET" reading takes its own receive frame. The job body
	 * receives on the second bus only inside the advance window, so on a
	 * non-window quantum nothing latches and the receive registers would still
	 * hold the previous window's value. The check drives receiveDspFrame itself
	 * to make the reading current, exactly as t0_chain_data_flow does.
	 *
	 * The quantum counts are derived from the Config and are not literals, and
	 * the product is where a literal went wrong. A quantum swaps at its own
	 * frame index, the swap advances the second bus only on a frame index the
	 * divider divides, and the mailbox is a ring of hopFrames + 1 frames -- so
	 * the frame reaches the read cell after hopFrames of THOSE ADVANCES, not
	 * after hopFrames quanta. The arrival is therefore on the
	 * hopFrames * secondBusFrameDivider-th quantum counted from the settling
	 * quantum's successor. The settling quantum itself stays a literal 1: frame
	 * index 0 is a window at every divider and is keyed to neither field.
	 *
	 * Deriving from this Config keeps the arm honest when a provisional constant
	 * moves; it establishes NOTHING about the hop or the divider the ADAPTER was
	 * handed, because this arm drives the default Config where both fields ARE
	 * the build constants and the comparison is a constant against itself. The
	 * off-window probe below exists only while the divider is at least 2: at 1
	 * every quantum is a window and the non-arrival has nothing to assert.
	 */
	void caseSecondBusForwarded(g2::Board& _board, g2::Executor& _executor)
	{
		g2::Scheduler::Config config;
		config.chainOrder = identityChainOrder();

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, _executor, _board, status);

		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
			"the default Config is accepted for the second-bus case");

		if(!scheduler)
		{
			check(false, "the second-bus case has a Scheduler");
			return;
		}

		g2::DspSet& set = _board.dspSet();

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			enableSecondTransmitter(set.peripherals(i).getEsai1());
			enableReceiver(set.peripherals(i).getEsai1());
		}

		/* Frame index 0 is a window quantum, so this settles the second bus at
		 * a known phase before anything is primed into it. */
		scheduler->runFrames(1);

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			dsp56k::Esai& source = set.peripherals(i).getEsai1();

			const dsp56k::TWord sample = 0x400000u + i + 1u;

			source.writeTX(0u, 0u);
			source.writeTX(2u, sample);
			checkEqual(g2::transmitDspFrame(source), source.getTxWordCount() + 1u,
				"the priming second-bus transmit frame ran");
			source.writeTX(2u, 0u);
		}

		/* Frame index 1: NOT a window quantum. */
		scheduler->runFrames(1);

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			dsp56k::Esai& sink = set.peripherals((i + 1u) % g2::kJobCount).getEsai1();

			char what[256];

			std::snprintf(what, sizeof(what),
				"the second-bus receive frame of position %u ran off the window",
				static_cast<unsigned>((i + 1u) % g2::kJobCount));
			checkEqual(g2::receiveDspFrame(sink), sink.getRxWordCount() + 1u, what);

			std::snprintf(what, sizeof(what),
				"position %u's second-bus frame has NOT reached position %u off the advance window",
				i, static_cast<unsigned>((i + 1u) % g2::kJobCount));
			checkEqual(sink.readRX(0u), 0u, what);
		}

		/* The remainder of the arrival count, the off-window probe above having
		 * spent the first of those quanta. */
		scheduler->runFrames(config.hopFrames * config.secondBusFrameDivider - 1u);

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			dsp56k::Esai& sink = set.peripherals((i + 1u) % g2::kJobCount).getEsai1();

			const dsp56k::TWord sample = 0x400000u + i + 1u;

			char what[256];
			std::snprintf(what, sizeof(what),
				"position %u's second-bus frame reached position %u on the next advance window",
				i, static_cast<unsigned>((i + 1u) % g2::kJobCount));
			checkEqual(sink.readRX(0u), sample, what);
		}
	}

	/* ---------------------------------------------------------------------
	 * CASE 5b. The second-bus divider at its other ordered configuration, and
	 * it is the arm that makes the argument's forwarding observable.
	 *
	 * WHY CASE 5 alone could not report it. Case 5 drives the DEFAULT Config,
	 * whose divider IS G2_SECOND_BUS_FRAME_DIVIDER -- so a Scheduler that
	 * handed the adapter that macro instead of the Config's value satisfies it
	 * at every position. MEASURED against this file as it stood WITHOUT this
	 * case: with the macro substituted for Config::secondBusFrameDivider at the
	 * adapter's construction, every case in it stayed green. The comparison was
	 * against a constant equal to itself.
	 *
	 * Why the context member does not cover it either. Case 7 asserts
	 * DspContext::secondBusFrameDivider at a non-default value, which is the
	 * value the JOB reads. The adapter's own copy is what the SWAP reads, and
	 * the two are separate forwardings of one Config field: the mutation above
	 * moves the swap's copy and leaves every context's untouched.
	 *
	 * The discriminator is a cadence. The swap advances the second bus only on
	 * a quantum whose frame index is a multiple of the divider. Frame index 1
	 * is such a quantum at a divider of 1 and is not one at the shipped 4, so
	 * a frame primed after the settling quantum crosses here and does not
	 * cross in case 5. The mutation and its red: hand the adapter
	 * G2_SECOND_BUS_FRAME_DIVIDER instead of the Config's value and this
	 * crossing stops happening, because the adapter then skips the quantum the
	 * Config asked it to advance on.
	 *
	 * A divider of 1 NEEDS Config::testOverride, which is the escape from the
	 * equality row and nothing else; the value is required in any case.
	 *
	 * The quantum count is derived and not a literal, for the reason case 3
	 * states: every quantum is an advance window at a divider of 1, so the
	 * count is the hop and nothing else.
	 *
	 * This reports the divider the SWAP reads and says nothing about the one
	 * each CONTEXT reads; those are two forwardings of one Config field. It says
	 * nothing about a DSP having run either -- every run gate is shut here, as
	 * it is everywhere in this file.
	 */
	void caseSecondBusDividerOneForwarded(g2::Board& _board, g2::Executor& _executor)
	{
		g2::Scheduler::Config config;
		config.chainOrder = identityChainOrder();
		config.secondBusFrameDivider = 1;
		config.testOverride          = true;

		check(config.secondBusFrameDivider != G2_SECOND_BUS_FRAME_DIVIDER,
			"the driven divider differs from the build constant (an arm that drove "
			"the constant would compare it against itself)");

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, _executor, _board, status);

		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
			"a divider of 1 with the override taken is accepted");

		if(!scheduler)
		{
			check(false, "a divider of 1 with the override taken yields a Scheduler");
			return;
		}

		g2::DspSet& set = _board.dspSet();

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			enableSecondTransmitter(set.peripherals(i).getEsai1());
			enableReceiver(set.peripherals(i).getEsai1());
		}

		/* Frame index 0 is a window quantum at EVERY divider, so this settles
		 * the second bus at a phase both arms share. */
		scheduler->runFrames(1);

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			dsp56k::Esai& source = set.peripherals(i).getEsai1();

			const dsp56k::TWord sample = 0x600000u + i + 1u;

			source.writeTX(0u, 0u);
			source.writeTX(2u, sample);
			checkEqual(g2::transmitDspFrame(source), source.getTxWordCount() + 1u,
				"the priming second-bus transmit frame ran at a divider of 1");
			source.writeTX(2u, 0u);
		}

		/* Frame index 1 onwards. NOT a window quantum at the shipped divider
		 * of 4; every one of them IS a window at a divider of 1, so the hop is
		 * the whole count and the job bodies' own receive frames latch what
		 * each swap delivered. */
		scheduler->runFrames(config.hopFrames);

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			dsp56k::Esai& sink = set.peripherals((i + 1u) % g2::kJobCount).getEsai1();

			const dsp56k::TWord sample = 0x600000u + i + 1u;

			char what[256];
			std::snprintf(what, sizeof(what),
				"at a divider of 1, position %u's second-bus frame reached position %u on a "
				"quantum the shipped divider would have skipped",
				i, static_cast<unsigned>((i + 1u) % g2::kJobCount));
			checkEqual(sink.readRX(0u), sample, what);
		}
	}

	/* ---------------------------------------------------------------------
	 * CASE 6. The second bus's topology is the Config's and not a literal.
	 *
	 * The two topologies differ at exactly one pair. A Ring gives the second
	 * bus dspCount mailboxes and position k writes (k + 1) mod N, so the tail
	 * writes the head's; a Line gives it dspCount + 1 and position k writes
	 * k + 1, so the tail writes a mailbox nothing reads and the head reads a
	 * mailbox nothing writes. Every other adjacent pair behaves identically.
	 * The wrap is therefore the whole discriminator, and case 5 above already
	 * asserts its PRESENCE at the default Ring.
	 *
	 * Why the seven adjacent pairs are asserted here too, and it is not
	 * decoration: they are what stops the wrap's absence from passing
	 * vacuously. A Scheduler that installed no chain callbacks at all would
	 * deliver nothing anywhere, and an absence assertion alone would call that
	 * a Line. The seven arrivals say the bus is running; the eighth says it is
	 * running as a Line.
	 *
	 * The mutation and its red: hand the adapter a literal ChainTopology::Ring
	 * and position 7's frame reaches position 0, so the wrap assertion fails.
	 *
	 * The quantum count is derived, by the ring-of-advances reasoning case 5
	 * states. This arm asserts no non-arrival, so it spends none of the count on
	 * an off-window probe and drives the whole product.
	 *
	 * This arm drives the default hop and the default divider, so the derivation
	 * reports neither field's forwarding.
	 */
	void caseSecondBusTopologyForwarded(g2::Board& _board, g2::Executor& _executor)
	{
		g2::Scheduler::Config config;
		config.chainOrder = identityChainOrder();
		config.secondBusTopology = g2::ChainTopology::Line;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, _executor, _board, status);

		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
			"a Line second bus is accepted (no rejection row constrains this field)");

		if(!scheduler)
		{
			check(false, "the Line-topology case has a Scheduler");
			return;
		}

		g2::DspSet& set = _board.dspSet();

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			enableSecondTransmitter(set.peripherals(i).getEsai1());
			enableReceiver(set.peripherals(i).getEsai1());
		}

		/* Frame index 0 is a window quantum, so this settles the second bus at
		 * a known phase before anything is primed into it. */
		scheduler->runFrames(1);

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			dsp56k::Esai& source = set.peripherals(i).getEsai1();

			const dsp56k::TWord sample = 0x500000u + i + 1u;

			source.writeTX(0u, 0u);
			source.writeTX(2u, sample);
			checkEqual(g2::transmitDspFrame(source), source.getTxWordCount() + 1u,
				"the priming second-bus transmit frame ran on the Line");
			source.writeTX(2u, 0u);
		}

		/* The arrival count, counted from the settling quantum's successor. Its
		 * last quantum is an advance window, so the job bodies' own receive
		 * frames latch what the swap delivered. */
		scheduler->runFrames(config.hopFrames * config.secondBusFrameDivider);

		for(unsigned i = 0; i + 1u < g2::kJobCount; ++i)
		{
			dsp56k::Esai& sink = set.peripherals(i + 1u).getEsai1();

			const dsp56k::TWord sample = 0x500000u + i + 1u;

			char what[256];
			std::snprintf(what, sizeof(what),
				"on a Line, position %u's second-bus frame reached position %u", i, i + 1u);
			checkEqual(sink.readRX(0u), sample, what);
		}

		/* The one assertion the two topologies disagree on, and its
		 * non-vacuity comes from the seven arrivals above and not from a stale
		 * reading. A carried reading cannot be relied on here: whatever a
		 * previous case left in position 0's second-bus receive register is
		 * gone by this point, because this case's own settling quantum runs
		 * dspJob for all eight slots and receiveDspFrame latches for any ESAI
		 * with an enabled receiver -- MEASURED, that register holds the
		 * previous case's sample on entry and reads zero once the settling
		 * quantum has run. So a zero here would also be produced by a bus that
		 * delivered nothing anywhere, and it is the seven arrivals against
		 * distinct non-zero samples that rule that out: they say the bus is
		 * running, and this one says it is running as a Line. */
		checkEqual(set.peripherals(0).getEsai1().readRX(0u), 0u,
			"on a Line, position 7's second-bus frame does NOT wrap to position 0");
	}

	/* ---------------------------------------------------------------------
	 * CASE 7. THE Config's second-bus divider and its DSP rate reach every
	 * context, asserted at values that are NOT the build constants.
	 *
	 * Why a second Scheduler rather than the one case 2 DRIVES. Case 2 reads
	 * both members off contexts built from a DEFAULT Config, where each field
	 * already equals the build constant it came from -- so a Scheduler that
	 * wrote G2_SECOND_BUS_FRAME_DIVIDER, or the two G2_DSP_CYCLES_PER_FRAME
	 * macros, straight into the context satisfies it. The comparison is
	 * against a constant equal to itself and it discriminates nothing.
	 *
	 * A divider of 2 needs Config::testOverride, which is the escape from the
	 * equality row and nothing else; the DSP rate needs no override, because
	 * the only rational the factory rejects is one with a zero denominator.
	 *
	 * The mutations and their red: write 4u (or the divider macro) into
	 * DspContext::secondBusFrameDivider and this case fails at every position;
	 * write the two DSP-rate macros into DspContext::rate and it fails at
	 * every position.
	 */
	void caseConfigValuesReachContexts(g2::Board& _board)
	{
		RecordingExecutor executor;

		g2::Scheduler::Config config;
		config.chainOrder = identityChainOrder();
		config.secondBusFrameDivider = 2;
		config.testOverride          = true;
		config.dspRate               = { 12345u, 67u };

		check(config.secondBusFrameDivider != G2_SECOND_BUS_FRAME_DIVIDER,
			"the driven divider differs from the build constant (a case that drove "
			"the constant would compare it against itself)");
		check(config.dspRate.num != G2_DSP_CYCLES_PER_FRAME_NUM
			&& config.dspRate.den != G2_DSP_CYCLES_PER_FRAME_DEN,
			"the driven DSP rate differs from the build constants in both terms");

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, _board, status);

		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
			"a divider of 2 with the override taken, and a non-default DSP rate, are accepted");

		if(!scheduler)
		{
			check(false, "the non-default Config yields a Scheduler");
			return;
		}

		scheduler->runFrames(1);

		checkEqual(executor.runs(), 1u, "the non-default Config's quantum dispatched once");

		if(executor.runs() != 1)
			return;

		const RecordingExecutor::Record& r = executor.record(0);

		checkEqual(r.count, g2::kJobCount, "the job array holds exactly kJobCount jobs");

		if(r.count != g2::kJobCount)
			return;

		for(unsigned i = 0; i < g2::kJobCount; ++i)
		{
			char what[256];

			std::snprintf(what, sizeof(what),
				"context %u carries the Config's NON-DEFAULT second-bus divider", i);
			checkEqual(r.ctx[i]->secondBusFrameDivider, config.secondBusFrameDivider, what);

			std::snprintf(what, sizeof(what),
				"context %u carries the Config's NON-DEFAULT DSP rate numerator", i);
			checkEqual(r.ctx[i]->rate.num, config.dspRate.num, what);

			std::snprintf(what, sizeof(what),
				"context %u carries the Config's NON-DEFAULT DSP rate denominator", i);
			checkEqual(r.ctx[i]->rate.den, config.dspRate.den, what);
		}
	}
}

int main()
{
	std::printf("t0_order: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	/* The board is declared first and every Scheduler below is a local or a
	 * unique_ptr declared after it. The rule is that the Board OUTLIVES
	 * the Scheduler: the factory hands the Scheduler the Board's own DSP set
	 * and every context borrows a core, two ESAI ports and a landed flag owned
	 * by a slot of it. Declaration order is what enforces that at a call site,
	 * and it is the only thing that does -- the factory says in as many words
	 * that it cannot check the lifetime of the referent. */
	g2::Board          board;
	RecordingExecutor  executor;
	RecordingTrace     trace;

	g2::Scheduler::Config config;
	config.chainOrder = identityChainOrder();
	config.trace      = &trace;

	g2::Status status{};

	const std::unique_ptr<g2::Scheduler> scheduler =
		g2::Scheduler::create(config, executor, board, status);

	if(!dsp56k::g_useJIT)
	{
		/* In an interpreter build no Scheduler can be created at all, which is
		 * the backend rule and not a skip this check invents. Asserting the
		 * refusal is the only claim this file may make in such a build. */
		check(scheduler == nullptr, "an interpreter build yields no Scheduler");
		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::BadBackend),
			"an interpreter build reports BadBackend");
	}
	else
	{
		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
			"the default Config is accepted");

		if(scheduler == nullptr)
		{
			check(false, "the default Config yields a Scheduler");
		}
		else
		{
			caseOrder(trace, executor, *scheduler);
			caseContexts(executor, board, config);
			caseChainCrossing(board, *scheduler, config);
		}
	}

	if(dsp56k::g_useJIT)
	{
		caseHopForwarded(board, executor);
		caseSecondBusForwarded(board, executor);
		caseSecondBusDividerOneForwarded(board, executor);
		caseSecondBusTopologyForwarded(board, executor);
		caseConfigValuesReachContexts(board);
	}

	if(g_failures != 0)
	{
		std::printf("t0_order: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_order: all %d cases passed\n", g_cases);
	return 0;
}
