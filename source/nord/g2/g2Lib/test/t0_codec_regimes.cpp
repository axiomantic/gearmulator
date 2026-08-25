/* t0_codec_regimes.cpp -- the check of SCH-21 step 2 (formerly SCH-22's own
 * Check: line). Design sections 13.5, 13.10 rule 3 and 13.10.5.
 *
 * THE TWO REGIMES, AND THE ONE SENTENCE THAT SEPARATES THEM. In the BOOT
 * regime a quantum runs the swap and the run phases only -- no ingress, no
 * egress, and NEITHER CODEC QUEUE TOUCHED. In the PLAY regime all four phases
 * run. The object knows its phase; the caller does not select it.
 *
 * WHAT ONLY THIS FILE CAN OBSERVE. SCH-19's t0_order names the five
 * unconditional phases and no more, because a Scheduler it builds has no
 * regime member and is the boot machine by construction -- so THE POSITION OF
 * THE INGRESS AND OF THE EGRESS in design section 13.5's order is established
 * here or nowhere. The two `TracePhase` enumerators SCH-19 declares are emitted
 * by nothing until this task lands, so a regime gate that never reaches the two
 * calls leaves a play trace five records long and the count below reports it.
 *
 * THE ORDERING CLAIM, STATED AS THE MUTATION THAT BREAKS IT: move the ingress
 * after the run phase, or the egress before it, and the play-regime comparison
 * fails at the first record that moved. Both mutations were performed and both
 * went red.
 *
 * THE NEGATIVE CASE IS THE WHOLE POINT OF THE ROW. With the play regime running
 * during what would be the boot, the sink fills, `CodecSink::push` refuses, and
 * the scheduler stops before the requested quanta are exhausted. That proves
 * the defect the boot regime closes is REAL and not a precaution, and it is
 * also the known positive for `droppedFrames` and `starvedFrames` -- the two
 * counters the boot-regime case requires to be zero.
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

	/* THE BOOT QUANTUM: five records, and the run phase is phases 1 .. 4. */
	constexpr g2::TracePhase kBootQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp
	};

	/* THE PLAY QUANTUM: seven records. THE INGRESS SITS AT INDEX 1, BEFORE THE
	 * WHOLE RUN PHASE, AND THE EGRESS AT INDEX 6, AFTER IT. That placement is
	 * the ordering claim, and comparing the whole sequence position by position
	 * is what makes it a property of this check rather than a sentence about
	 * one: a moved record shifts every later tag and the first comparison
	 * reports it. */
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

	constexpr unsigned kLookahead    = 4;   /* L */
	constexpr unsigned kMaxHostBlock = 3;   /* B */
	constexpr unsigned kDspCount     = static_cast<unsigned>(g2::kJobCount);

	/* BOTH QUEUE CAPACITIES ARE L + B, design section 13.6.1, and the boot run
	 * is TWICE that -- more quanta than either queue could hold, which is what
	 * makes "neither queue was touched" a claim with teeth. Both are derived
	 * from the two Config fields and neither is a literal. */
	constexpr size_t   kCapacity  = static_cast<size_t>(kLookahead) + kMaxHostBlock;
	constexpr unsigned kBootQuanta = static_cast<unsigned>(2 * kCapacity);

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
}

int main()
{
	std::printf("t0_codec_regimes: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	if(!dsp56k::g_useJIT)
	{
		g2::Board          board;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		check(scheduler == nullptr, "an interpreter build yields no Scheduler");
		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::BadBackend),
			"an interpreter build reports BadBackend");

		std::printf("t0_codec_regimes: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return g_failures == 0 ? 0 : 1;
	}

	g2::Board          board;
	g2::SerialExecutor executor;
	RecordingTrace     trace;

	g2::Scheduler::Config config;
	config.lookaheadFrames    = kLookahead;
	config.maxHostBlockFrames = kMaxHostBlock;
	config.trace              = &trace;

	g2::Status status{};

	const std::unique_ptr<g2::Scheduler> scheduler =
		g2::Scheduler::create(config, executor, board, status);

	checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
		"the synthetic machine's Config is accepted");

	if(!scheduler)
	{
		check(false, "the Config yields a Scheduler");
		std::printf("t0_codec_regimes: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	g2::Scheduler& s = *scheduler;

	/* -----------------------------------------------------------------
	 * CASE 1. THE BOOT REGIME. 2 x (L + B) quanta -- more than either queue
	 * holds -- and every one of them runs to completion.
	 *
	 * THE FRAME INDEX IS THE FIRST HALF OF THE PROOF. Had the boot regime
	 * touched the sink, it would have filled after L + B pushes and the
	 * scheduler would have stopped, exactly as case 4 below shows it does when
	 * the play regime IS running. So a frame index of 2 x (L + B) is the
	 * statement "the sink never filled, because the sink was never written". */
	s.runFrames(kBootQuanta);

	checkEqual(s.frameIndex(), kBootQuanta,
		"every boot quantum ran: the boot regime never touches the sink, so the sink cannot fill");

	checkEqual(trace.count(), static_cast<uint64_t>(kBootQuanta) * kBootPhases,
		"a boot quantum emits exactly the five unconditional phases -- no ingress, no egress");

	checkQuanta(trace, 0, kBootQuanta, kBootQuantum, kBootPhases, 0, "boot");

	/* -----------------------------------------------------------------
	 * CASE 2. NEITHER CODEC QUEUE WAS TOUCHED.
	 *
	 * The two queue DEPTHS are read through the declared surface: push()
	 * returns the frames the CodecSource accepted and pull() returns the
	 * frames the CodecSink supplied, so a request of capacity + 1 on each
	 * measures free space and depth exactly. BOTH PROBES MUTATE, so the four
	 * counter assertions run FIRST.
	 *
	 * ALL FOUR CODEC-QUEUE COUNTERS ARE ZERO, and none of these four is a
	 * counter nothing can move: case 4 drives starvedFrames and droppedFrames
	 * off zero on this same object, and the two probes below drive
	 * overflowFrames and underflowFrames off zero as their last act.
	 */
	checkEqual(s.starvedFrames(),   0u, "the boot regime consumed no source frame");
	checkEqual(s.overflowFrames(),  0u, "the boot regime pushed nothing at the source");
	checkEqual(s.droppedFrames(),   0u, "the boot regime pushed no sink frame");
	checkEqual(s.underflowFrames(), 0u, "the boot regime took nothing from the sink");

	/* THE TWO CHAIN UNDERRUN COUNTERS, AND A SPEC DISAGREEMENT STATED RATHER
	 * THAN PAPERED OVER. SCH-21 step 2 asks that "both underrun counters" also
	 * read zero after the boot run. THEY CANNOT IN THIS FIXTURE, and the
	 * reason is structural rather than a defect: no firmware is downloaded, so
	 * every slot's run gate is shut, no transmit callback ever fires, and
	 * CHN-7's advanceAll counts an audio-bus underrun at EVERY position for
	 * EVERY quantum by construction. The zero the row asks for belongs to a
	 * machine whose DSPs are producing -- which is milestone M5's `--impulse`
	 * row, not this synthetic one.
	 *
	 * WHAT IS ASSERTED INSTEAD IS AN EQUALITY AND NOT A SHRUG: exactly one
	 * audio-bus underrun for each quantum, and exactly one second-bus underrun
	 * for each second-bus WINDOW quantum, at every position. Both expectations
	 * are derived -- from the boot count and from the Config's own divider --
	 * and both would move if the boot regime ran a different number of quanta
	 * than case 1 asserts. */
	for(unsigned p = 0; p < kDspCount; ++p)
	{
		char what[256];

		std::snprintf(what, sizeof(what),
			"boot: underrunFrames(%u) counts one audio-bus underrun for each quantum", p);
		checkEqual(s.underrunFrames(p), kBootQuanta, what);

		std::snprintf(what, sizeof(what),
			"boot: secondBusUnderrunFrames(%u) counts one underrun for each window quantum", p);
		checkEqual(s.secondBusUnderrunFrames(p),
			windowQuanta(0, kBootQuanta, config.secondBusFrameDivider), what);

		std::snprintf(what, sizeof(what),
			"boot: phaseErrorFrames(%u): the scheduler asked for every transmit that fired", p);
		checkEqual(s.phaseErrorFrames(p), 0u, what);
	}

	/* -----------------------------------------------------------------
	 * CASE 3. THE HAND-OFF, AND THE SCH-21 STATE IT LEAVES.
	 */
	trace.clear();

	s.beginPlayPhase();

	checkEqual(trace.count(), static_cast<uint64_t>(kLookahead) * kPlayPhases,
		"beginPlayPhase runs exactly L quanta and each one is a PLAY quantum of seven records");

	checkQuanta(trace, 0, kLookahead, kPlayQuantum, kPlayPhases, kBootQuanta, "play");

	checkEqual(s.frameIndex(), static_cast<uint64_t>(kBootQuanta) + kLookahead,
		"the frame index is bootQuanta + L");

	checkEqual(s.starvedFrames(), 0u,
		"the priming run consumed exactly the L frames step 2 primed and starved on none");

	/* -----------------------------------------------------------------
	 * CASE 4. THE NEGATIVE CASE, AND IT IS THE WHOLE POINT OF THE ROW.
	 *
	 * The play regime is now running. Ask for the SAME 2 x (L + B) quanta the
	 * boot run completed. The sink holds L, its capacity is L + B, and each
	 * play quantum pushes one frame into it -- so exactly B pushes succeed, the
	 * next one is REFUSED, and the scheduler stops. The quantum that was
	 * refused still ran to completion, so it advances the frame index too:
	 * B + 1 quanta, and not the 2 x (L + B) that were asked for.
	 *
	 * THAT IS THE DEFECT THE BOOT REGIME CLOSES, DEMONSTRATED RATHER THAN
	 * DESCRIBED: without the boot regime this is what a boot would have done.
	 */
	{
		const uint64_t before = s.frameIndex();

		s.runFrames(kBootQuanta);

		const uint64_t ran = s.frameIndex() - before;

		checkEqual(ran, static_cast<uint64_t>(kMaxHostBlock) + 1,
			"the play regime fills the sink and STOPS: B pushes fit, the next is refused");

		check(ran < kBootQuanta,
			"the scheduler stopped before the requested quanta were exhausted");

		checkEqual(s.droppedFrames(), 1u,
			"KNOWN POSITIVE: the refused frame was counted and NOT overwritten");

		checkEqual(s.starvedFrames(), ran,
			"KNOWN POSITIVE: every play quantum past the priming consumed a zero frame the host never supplied");
	}

	/* -----------------------------------------------------------------
	 * CASE 5. THE TWO QUEUE DEPTHS, MEASURED LAST BECAUSE BOTH PROBES MUTATE.
	 *
	 * The sink is FULL at this point -- that is what case 4 established -- so a
	 * pull of capacity + 1 takes the whole capacity and falls one short.
	 */
	{
		std::vector<g2::Frame> out(kCapacity + 1);

		const size_t pulled = s.pull(out.data(), kCapacity + 1);

		checkEqual(pulled, kCapacity, "the sink was FULL when the scheduler stopped");

		checkEqual(s.underflowFrames(), 1u,
			"KNOWN POSITIVE: the one frame the sink could not supply raised underflowFrames");
	}

	{
		const std::vector<g2::Frame> in(kCapacity + 1);

		const size_t pushed = s.push(in.data(), kCapacity + 1);

		checkEqual(pushed, kCapacity, "the source was EMPTY: it accepts its whole capacity");

		checkEqual(s.overflowFrames(), 1u,
			"KNOWN POSITIVE: the one frame past capacity was refused and counted");
	}

	if(g_failures != 0)
	{
		std::printf("t0_codec_regimes: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_codec_regimes: all %d cases passed\n", g_cases);
	return 0;
}
