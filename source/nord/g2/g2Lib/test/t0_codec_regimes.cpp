/* The two regimes, and the one sentence that separates them. In the boot regime
 * a quantum runs the swap and the run phases only -- no ingress, no egress, and
 * neither codec queue touched. In the play regime all four phases run. The
 * object knows its phase; the caller does not select it.
 *
 * The position of the ingress and of the egress in the phase order is
 * established here or nowhere. A regime gate that never reaches the two calls
 * leaves a play trace of the boot length, and the count below reports it.
 *
 * The ordering claim, stated as the mutation that breaks it: move the ingress
 * after the run phase, or the egress before it, and the play-regime comparison
 * fails at the first record that moved.
 *
 * The negative case: with the play regime running during what would be the boot,
 * the sink fills, `CodecSink::push` refuses, and the scheduler stops before the
 * requested quanta are exhausted. That proves the defect the boot regime closes
 * is real and not a precaution, and it is also the known positive for
 * `droppedFrames` and `starvedFrames` -- the two counters the boot-regime case
 * requires to be zero.
 *
 * No case here is a language assert() and no case catches an exception, so this
 * file reports identically under NDEBUG and without it.
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

	constexpr g2::TracePhase kBootQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp
	};

	/* The ingress sits before the whole run phase and the egress after it. That
	 * placement is the ordering claim, and the whole sequence is compared
	 * position by position: a moved record shifts every later tag and the first
	 * comparison reports it. */
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

	/* Both queue capacities are L + B, and the boot run is twice that -- more
	 * quanta than either queue could hold, which is what makes "neither queue was
	 * touched" a claim with teeth. Both are derived from the two Config fields
	 * and neither is a literal. */
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
	 * The boot regime. 2 x (L + B) quanta -- more than either queue holds -- and
	 * every one of them runs to completion.
	 *
	 * Had the boot regime touched the sink, it would have filled after L + B
	 * pushes and the scheduler would have stopped, as it does below when the play
	 * regime is running. So a frame index of 2 x (L + B) is the statement "the
	 * sink never filled, because the sink was never written". */
	s.runFrames(kBootQuanta);

	checkEqual(s.frameIndex(), kBootQuanta,
		"every boot quantum ran: the boot regime never touches the sink, so the sink cannot fill");

	checkEqual(trace.count(), static_cast<uint64_t>(kBootQuanta) * kBootPhases,
		"a boot quantum emits exactly the five unconditional phases -- no ingress, no egress");

	checkQuanta(trace, 0, kBootQuanta, kBootQuantum, kBootPhases, 0, "boot");

	/* -----------------------------------------------------------------
	 * Neither codec queue was touched.
	 *
	 * The two queue depths are read through the declared surface: push() returns
	 * the frames the CodecSource accepted and pull() returns the frames the
	 * CodecSink supplied, so a request of capacity + 1 on each measures free
	 * space and depth exactly. Both probes mutate, so the counter assertions run
	 * first.
	 *
	 * None of the codec-queue counters asserted zero here is a counter nothing
	 * can move: the negative case below drives starvedFrames and droppedFrames
	 * off zero on this same object, and the two probes below drive overflowFrames
	 * and underflowFrames off zero as their last act.
	 */
	checkEqual(s.starvedFrames(),   0u, "the boot regime consumed no source frame");
	checkEqual(s.overflowFrames(),  0u, "the boot regime pushed nothing at the source");
	checkEqual(s.droppedFrames(),   0u, "the boot regime pushed no sink frame");
	checkEqual(s.underflowFrames(), 0u, "the boot regime took nothing from the sink");

	/* The two chain underrun counters cannot read zero in this fixture, and the
	 * reason is structural rather than a defect: no firmware is downloaded, so
	 * every slot's run gate is shut, no transmit callback ever fires, and
	 * advanceAll counts an audio-bus underrun at every position for every quantum
	 * by construction. A zero here belongs to a machine whose DSPs are producing.
	 *
	 * What is asserted instead is an equality: exactly one audio-bus underrun for
	 * each quantum, and exactly one second-bus underrun for each second-bus
	 * window quantum, at every position. Both expectations are derived -- from
	 * the boot count and from the Config's own divider -- and both would move if
	 * the boot regime ran a different number of quanta. */
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
	 * The hand-off, and the state it leaves.
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
	 * The negative case.
	 *
	 * The play regime is now running. Ask for the same 2 x (L + B) quanta the
	 * boot run completed. The sink holds L, its capacity is L + B, and each play
	 * quantum pushes one frame into it -- so exactly B pushes succeed, the next
	 * one is refused, and the scheduler stops. The quantum that was refused still
	 * ran to completion, so it advances the frame index too: B + 1 quanta, and
	 * not the 2 x (L + B) that were asked for.
	 *
	 * Without the boot regime, this is what a boot would have done.
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
	 * The two queue depths, measured last because both probes mutate.
	 *
	 * The sink is full at this point, so a pull of capacity + 1 takes the whole
	 * capacity and falls one short.
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
