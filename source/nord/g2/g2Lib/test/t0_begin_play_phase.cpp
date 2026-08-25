/* t0_begin_play_phase.cpp -- the check of SCH-21 step 1 (the block's own
 * former Check: line). Design sections 13.6.1, 13.10 rule 3 and 13.10.5.
 *
 * WHAT beginPlayPhase IS. It is the boot-to-play transition, as one call, and
 * design section 13.10 rule 3 states its five steps in order:
 *
 *   1  Clear both codec queues.
 *   2  Push exactly lookaheadFrames zero frames into the CodecSource.
 *   3  Zero all seven chain-health counters and both diagnostic counters,
 *      BEFORE step 4, so the priming run's own counters stay visible.
 *   4  Run exactly lookaheadFrames quanta in the PLAY regime.
 *   5  Clear the recorded owning-thread identity.
 *
 * It touches no emulated machine state, clears no fault, and does not reset
 * the virtual frame index -- its own quanta advance it by L.
 *
 * THE HAND-OFF STATE THIS FILE ASSERTS, EXACTLY: the CodecSource holds 0
 * frames, the CodecSink holds exactly L, all seven chain-health counters are
 * 0, and the frame index is bootQuanta + L.
 *
 * EVERY "IS ZERO" ASSERTION HERE IS GUARDED BY A KNOWN POSITIVE THAT RAN
 * FIRST, and that is the whole reason the boot run below is 37 quanta rather
 * than one. A counter that was never able to leave zero makes "it is zero
 * afterwards" a comparison of 0 against 0, which no mutation of the zeroing
 * step could turn red. The guards are named at each case.
 *
 * THE MCU RATE IS 0/1 AND THAT IS A MEASURING INSTRUMENT, NOT A MACHINE
 * CONFIGURATION. With a numerator of zero the section 13.4.6 block's budget is
 * zero at every quantum, so `want <= 0` holds at every quantum, the role
 * filler is never invoked and longDispatchQuanta(0) counts QUANTA RUN SINCE
 * THE LAST ZEROING, exactly. That turns step 3's "zero the two diagnostic
 * counters" and step 4's "exactly L quanta" into two exact equalities instead
 * of two bounds. A denominator of zero is the only rejected rational
 * (Status::BadRational), so 0/1 is a legal Config.
 *
 * NO CASE HERE IS A LANGUAGE assert() AND NO CASE CATCHES AN EXCEPTION, so
 * this file reports identically under NDEBUG and without it.
 */

#include "board.h"
#include "codecQueues.h"
#include "executor.h"
#include "frame.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace
{
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

	/* THE TRACE SINK. It records the phase tag and the frame index of every
	 * phase, in the order the Scheduler emits them. It is the only seam that
	 * reaches the phases that run serially inside the Scheduler. */
	class RecordingTrace final : public g2::TraceSink
	{
	public:
		static constexpr size_t kMax = 4096;

		void onPhase(const g2::TracePhase _phase, const uint64_t _frameIndex) noexcept override
		{
			if(m_count < kMax)
			{
				m_phase[m_count] = _phase;
				m_frame[m_count] = _frameIndex;
			}
			++m_count;
		}

		size_t         count()               const { return m_count; }
		g2::TracePhase phase(const size_t i) const { return m_phase[i]; }
		uint64_t       frame(const size_t i) const { return m_frame[i]; }
		void           clear()                     { m_count = 0; }

	private:
		size_t         m_count = 0;
		g2::TracePhase m_phase[kMax]{};
		uint64_t       m_frame[kMax]{};
	};

	/* THE FIVE UNCONDITIONAL PHASES -- the whole of a BOOT quantum. */
	constexpr g2::TracePhase kBootQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp
	};

	/* THE SEVEN PHASES OF A PLAY QUANTUM. The ingress precedes the whole run
	 * phase and the egress follows it. */
	constexpr g2::TracePhase kPlayQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Ingress,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp,
		g2::TracePhase::Egress
	};

	constexpr size_t kBootPhases = sizeof(kBootQuantum) / sizeof(kBootQuantum[0]);
	constexpr size_t kPlayPhases = sizeof(kPlayQuantum) / sizeof(kPlayQuantum[0]);

	constexpr unsigned kLookahead    = 4;
	constexpr unsigned kMaxHostBlock = 3;
	constexpr unsigned kBootQuanta   = 37;
	constexpr unsigned kDspCount     = static_cast<unsigned>(g2::kJobCount);

	/* THE CAPACITY IS DERIVED FROM THE TWO Config FIELDS AND NEVER WRITTEN AS
	 * A LITERAL. Design section 13.6.1 fixes both queue capacities at
	 * lookaheadFrames + B, so a literal here would be the rule copied rather
	 * than the rule read. */
	constexpr size_t kCapacity = static_cast<size_t>(kLookahead) + kMaxHostBlock;

	/* Asserts one whole trace against a quantum table, position by position,
	 * starting at trace record `_at` and frame index `_firstFrame`. An omitted
	 * or moved phase shifts every later tag and the first comparison reports
	 * it. Returns the record index just past the quanta it consumed. */
	size_t checkQuanta(const RecordingTrace& _trace, const size_t _at, const size_t _quanta,
		const g2::TracePhase* const _table, const size_t _phases, const uint64_t _firstFrame,
		const char* const _regime)
	{
		for(size_t q = 0; q < _quanta; ++q)
		{
			for(size_t p = 0; p < _phases; ++p)
			{
				const size_t at = _at + q * _phases + p;

				if(at >= _trace.count())
				{
					check(false, "the trace holds every phase the regime should have emitted");
					return _trace.count();
				}

				char what[256];

				std::snprintf(what, sizeof(what), "%s quantum %zu phase %zu is %s (observed %s)",
					_regime, q, p, phaseName(_table[p]), phaseName(_trace.phase(at)));
				check(_trace.phase(at) == _table[p], what);

				std::snprintf(what, sizeof(what), "%s quantum %zu phase %s carries frame index %llu",
					_regime, q, phaseName(_table[p]),
					static_cast<unsigned long long>(_firstFrame + q));
				checkEqual(_trace.frame(at), _firstFrame + q, what);
			}
		}

		return _at + _quanta * _phases;
	}

	/* THE NUMBER OF SECOND-BUS WINDOW QUANTA in the half-open frame-index range
	 * [_from, _from + _count). The second bus advances -- and therefore counts
	 * an underrun -- only when frameIndex % divider == 0, so the expected
	 * second-bus reading is DERIVED from the divider the Config carries and is
	 * never written here as a literal. */
	uint64_t windowQuanta(const uint64_t _from, const uint64_t _count, const unsigned _divider)
	{
		uint64_t n = 0;

		for(uint64_t f = _from; f < _from + _count; ++f)
		{
			if(f % _divider == 0)
				++n;
		}

		return n;
	}

	/* Reads back the seven chain-health counters of design section 13.10.5's
	 * observability block. THE THREE PER-POSITION COUNTERS ARE ASSERTED AT
	 * EVERY POSITION, because a zeroing that missed one position would
	 * otherwise pass. */
	void checkSevenCounters(const g2::Scheduler& _s, const uint64_t _underrun,
		const uint64_t _secondUnderrun, const uint64_t _phaseError,
		const uint64_t _starved, const uint64_t _overflow,
		const uint64_t _dropped, const uint64_t _underflow, const char* const _when)
	{
		for(unsigned p = 0; p < kDspCount; ++p)
		{
			char what[256];

			std::snprintf(what, sizeof(what), "%s: underrunFrames(%u)", _when, p);
			checkEqual(_s.underrunFrames(p), _underrun, what);

			std::snprintf(what, sizeof(what), "%s: secondBusUnderrunFrames(%u)", _when, p);
			checkEqual(_s.secondBusUnderrunFrames(p), _secondUnderrun, what);

			std::snprintf(what, sizeof(what), "%s: phaseErrorFrames(%u)", _when, p);
			checkEqual(_s.phaseErrorFrames(p), _phaseError, what);
		}

		char what[256];

		std::snprintf(what, sizeof(what), "%s: starvedFrames", _when);
		checkEqual(_s.starvedFrames(), _starved, what);

		std::snprintf(what, sizeof(what), "%s: overflowFrames", _when);
		checkEqual(_s.overflowFrames(), _overflow, what);

		std::snprintf(what, sizeof(what), "%s: droppedFrames", _when);
		checkEqual(_s.droppedFrames(), _dropped, what);

		std::snprintf(what, sizeof(what), "%s: underflowFrames", _when);
		checkEqual(_s.underflowFrames(), _underflow, what);
	}
}

int main()
{
	std::printf("t0_begin_play_phase: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	if(!dsp56k::g_useJIT)
	{
		/* In an interpreter build no Scheduler can be created at all
		 * (section 11.4.3), so the refusal is the only claim this file may
		 * make. */
		g2::Board          board;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;
		config.backend = g2::Backend::Jit;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		check(scheduler == nullptr, "an interpreter build yields no Scheduler");
		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::BadBackend),
			"an interpreter build reports BadBackend");

		std::printf("t0_begin_play_phase: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return g_failures == 0 ? 0 : 1;
	}

	/* THE BOARD IS DECLARED FIRST. Every context borrows a core, two ESAI
	 * ports and a landed flag owned by a slot of the Board's DSP set, so the
	 * Board must OUTLIVE the Scheduler and declaration order is the only thing
	 * that enforces it. */
	g2::Board          board;
	g2::SerialExecutor executor;
	RecordingTrace     trace;

	g2::Scheduler::Config config;
	config.lookaheadFrames    = kLookahead;
	config.maxHostBlockFrames = kMaxHostBlock;
	config.mcuRate            = { 0, 1 };
	config.trace              = &trace;

	g2::Status status{};

	const std::unique_ptr<g2::Scheduler> scheduler =
		g2::Scheduler::create(config, executor, board, status);

	checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
		"a lookahead of 4, a host block of 3 and an MCU rate of 0/1 are accepted");

	if(!scheduler)
	{
		check(false, "the Config yields a Scheduler");
		std::printf("t0_begin_play_phase: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	g2::Scheduler& s = *scheduler;

	/* -----------------------------------------------------------------
	 * CASE 1. A fresh Scheduler is the BOOT machine, and the boot run is
	 * what arms every known positive the later cases need.
	 */
	checkEqual(s.frameIndex(), 0u, "a fresh Scheduler is at frame 0");

	s.runFrames(kBootQuanta);

	checkEqual(s.frameIndex(), kBootQuanta, "the boot run advanced the frame index by its own count");

	checkEqual(trace.count(), static_cast<uint64_t>(kBootQuanta) * kBootPhases,
		"a boot quantum emits exactly the five unconditional phases");

	checkQuanta(trace, 0, kBootQuanta, kBootQuantum, kBootPhases, 0, "boot");

	/* -----------------------------------------------------------------
	 * CASE 2. THE KNOWN POSITIVES. Each of these asserts that a counter the
	 * next case will require to be ZERO is able to be NON-zero first, which
	 * is what stops case 4 from comparing 0 against 0.
	 *
	 * underrunFrames: no firmware is downloaded, so every slot's run gate is
	 * shut, no transmit callback ever fires and advanceAll counts an audio-bus
	 * underrun at EVERY position for EVERY quantum. The count is therefore
	 * exactly the number of boot quanta -- an equality, not a bound.
	 *
	 * longDispatchQuanta(0): the MCU rate is 0/1, so the section 13.4.6
	 * block's `want <= 0` branch is taken at every quantum and the counter is
	 * exactly the number of quanta run.
	 */
	for(unsigned p = 0; p < kDspCount; ++p)
	{
		char what[256];

		std::snprintf(what, sizeof(what),
			"KNOWN POSITIVE: underrunFrames(%u) counts one underrun for each boot quantum", p);
		checkEqual(s.underrunFrames(p), kBootQuanta, what);
	}

	for(unsigned p = 0; p < kDspCount; ++p)
	{
		char what[256];

		std::snprintf(what, sizeof(what),
			"KNOWN POSITIVE: secondBusUnderrunFrames(%u) counts one underrun for each window quantum", p);
		checkEqual(s.secondBusUnderrunFrames(p),
			windowQuanta(0, kBootQuanta, config.secondBusFrameDivider), what);
	}

	checkEqual(s.longDispatchQuanta(0), kBootQuanta,
		"KNOWN POSITIVE: the MCU context's longDispatchQuanta counts every boot quantum at rate 0/1");

	/* THE BOOT REGIME TOUCHES NEITHER QUEUE, which is the property SCH-22's
	 * boot regime exists to deliver and which case 4's "the source is empty"
	 * would otherwise be unable to distinguish from "the boot drained it". */
	checkEqual(s.starvedFrames(), 0u, "the boot regime consumed no source frame");
	checkEqual(s.droppedFrames(), 0u, "the boot regime pushed no sink frame");

	/* THE OWNING THREAD IS RECORDED BY THE FIRST runFrames. */
	check(s.owningThread() == std::this_thread::get_id(),
		"KNOWN POSITIVE: the boot run recorded this thread as the owner");

	/* -----------------------------------------------------------------
	 * CASE 3. beginPlayPhase, and the trace it emits.
	 *
	 * Step 4 runs exactly L quanta and they run in the PLAY regime, so the
	 * records it adds are exactly L x seven, in the play order. A regime gate
	 * that never reaches the two calls leaves each of these quanta five
	 * records long and the count below reports it.
	 */
	trace.clear();

	scheduler->beginPlayPhase();

	checkEqual(trace.count(), static_cast<uint64_t>(kLookahead) * kPlayPhases,
		"beginPlayPhase runs exactly lookaheadFrames quanta, each in the play regime");

	checkQuanta(trace, 0, kLookahead, kPlayQuantum, kPlayPhases, kBootQuanta, "priming");

	/* -----------------------------------------------------------------
	 * CASE 4. THE HAND-OFF STATE, ASSERTED EXACTLY.
	 */
	checkEqual(s.frameIndex(), static_cast<uint64_t>(kBootQuanta) + kLookahead,
		"the frame index is bootQuanta + L: beginPlayPhase does not reset it and its own quanta advance it");

	/* THE SEVEN COUNTERS AFTER THE HAND-OFF, AND THE SPEC'S TWO CLAUSES ABOUT
	 * THEM DISAGREE IN THIS FIXTURE. THE MORE SPECIFIC ONE WINS AND THE
	 * DISAGREEMENT IS STATED RATHER THAN PAPERED OVER.
	 *
	 *   Clause A, the hand-off state: "all seven chain-health counters 0".
	 *   Clause B, step 3: the zeroing happens BEFORE the priming run of step 4,
	 *   "so that priming-run underruns stay visible ... A DSP that underruns
	 *   during priming is a real finding, and clearing afterwards would hide
	 *   it."
	 *
	 * The two agree only when the priming run is CLEAN. THIS FIXTURE DOWNLOADS
	 * NO FIRMWARE, so every slot's run gate is shut, no transmit callback ever
	 * fires, and each of the L priming quanta is a real audio-bus underrun at
	 * every position. Clause A therefore cannot hold here, and clause B says in
	 * as many words that the reading must survive. Asserting zero would require
	 * the implementation to clear AFTER the priming run, which is the exact
	 * defect clause B names.
	 *
	 * WHAT IS ASSERTED INSTEAD IS STRICTLY STRONGER THAN ZERO, because it
	 * separates three implementations that "== 0" cannot:
	 *
	 *   zeroed before step 4  ->  L               (correct)
	 *   zeroed after step 4   ->  0
	 *   never zeroed          ->  bootQuanta + L
	 *
	 * Every expected value below is DERIVED -- from L, from the boot count and
	 * from the Config's own second-bus divider -- and none is a literal. */
	{
		const uint64_t expectedSecondBus =
			windowQuanta(kBootQuanta, kLookahead, config.secondBusFrameDivider);

		checkSevenCounters(s,
			/* underrunFrames        */ kLookahead,
			/* secondBusUnderrun     */ expectedSecondBus,
			/* phaseErrorFrames      */ 0,
			/* starvedFrames         */ 0,
			/* overflowFrames        */ 0,
			/* droppedFrames         */ 0,
			/* underflowFrames       */ 0,
			"after beginPlayPhase");

		/* THE SECOND-BUS EXPECTATION IS ONLY A DISCRIMINATOR IF IT IS NOT
		 * ZERO. At divider 4 and a boot of 37 the priming range is
		 * [37, 41), which holds exactly one window quantum -- frame 40 -- so
		 * this guard is what stops the row above from being 0 == 0. */
		check(expectedSecondBus > 0,
			"KNOWN POSITIVE: the priming run covers at least one second-bus window quantum");

		/* THE PRIMING RUN CONSUMED EXACTLY THE L FRAMES STEP 2 PRIMED. Had
		 * step 2 pushed fewer, the source would have run dry and starvedFrames
		 * would name the shortfall; the zero above is therefore a statement
		 * about step 2 and step 4 agreeing, not a counter nothing can move --
		 * t0_codec_regimes drives the same counter off zero. */
	}

	/* STEP 3 HAPPENS BEFORE STEP 4, AND THIS IS THE ASSERTION THAT SAYS SO.
	 * At an MCU rate of 0/1 the counter equals the quanta run since the last
	 * zeroing. Zeroing before the priming run leaves exactly L; zeroing after
	 * it would leave 0; not zeroing at all would leave bootQuanta + L. The
	 * three are distinct values, so this one equality discriminates all
	 * three. */
	checkEqual(s.longDispatchQuanta(0), kLookahead,
		"the two diagnostic counters were zeroed BEFORE the priming run, and the priming run was L quanta");

	/* STEP 5. */
	check(s.owningThread() == std::thread::id{},
		"beginPlayPhase cleared the recorded owning-thread identity");

	/* -----------------------------------------------------------------
	 * CASE 5. THE TWO QUEUE DEPTHS, MEASURED THROUGH THE DECLARED SURFACE.
	 *
	 * Design section 13.10.5 declares no queue-depth accessor, and this file
	 * adds none. push() returns the frames the CodecSource ACCEPTED and pull()
	 * returns the frames the CodecSink SUPPLIED, so a request of capacity + 1
	 * on each measures the free space and the depth exactly.
	 *
	 * BOTH PROBES MUTATE, so they run LAST -- after every counter assertion
	 * above. Each also drives one counter off zero, which is the known
	 * positive for the two counters no other case here can move:
	 * underflowFrames and overflowFrames.
	 */
	{
		std::vector<g2::Frame> out(kCapacity + 1);

		const size_t pulled = s.pull(out.data(), kCapacity + 1);

		checkEqual(pulled, kLookahead,
			"the CodecSink holds exactly lookaheadFrames frames at the hand-off");

		checkEqual(s.underflowFrames(), static_cast<uint64_t>(kCapacity) + 1 - kLookahead,
			"KNOWN POSITIVE: a pull the sink could not satisfy raises underflowFrames by the shortfall");
	}

	{
		const std::vector<g2::Frame> in(kCapacity + 1);

		const size_t pushed = s.push(in.data(), kCapacity + 1);

		checkEqual(pushed, kCapacity,
			"the CodecSource is EMPTY at the hand-off: it accepts its whole capacity");

		checkEqual(s.overflowFrames(), 1u,
			"KNOWN POSITIVE: the one frame past capacity was refused and counted");
	}

	if(g_failures != 0)
	{
		std::printf("t0_begin_play_phase: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_begin_play_phase: all %d cases passed\n", g_cases);
	return 0;
}
