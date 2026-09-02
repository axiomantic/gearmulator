/* The row's own order, for each virtual frame:
 *
 *   1. Swap     advance() every audio-bus mailbox on every quantum; every
 *               second-bus mailbox only when frameIndex % secondBusFrameDivider
 *               == 0. This is the single swap point for the quantum, and
 *               nothing else moves a mailbox head.
 *   2. Ingress  write the codec source stereo pair into slots 0 and 1 of
 *               mailbox 0's read frame, through ingressFrame() and not through
 *               read(), which is const.
 *   3. Run      the MCU context, then DSP 0 to DSP 7, in the fixed order.
 *   4. Egress   read slots 0 and 1 of the tail mailbox's write frame, through
 *               egressFrame() and not through write().
 *
 * Steps 2 and 4 are the only accesses that break the run-phase invariant, and
 * they break it in a defined way. Both belong to the chain adapter, both are
 * codec-facing, and both are strictly ordered against the run phase. Their
 * effect is that the two codec edges carry no delay of their own, which is
 * what the hardware does.
 *
 * A Ring has no ingress and no egress phase, so steps 2 and 4 do not run for
 * it.
 */

#include "board.h"
#include "chainAdapter.h"
#include "dspContext.h"
#include "executor.h"
#include "frame.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/audio.h"
#include "dsp56kEmu/dsp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

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
			static_cast<unsigned long long>(_observed),
			static_cast<unsigned long long>(_expected));
		++g_failures;
	}

	void checkEqualHex(const int32_t _observed, const int32_t _expected, const char* const _what)
	{
		++g_cases;

		if(_observed == _expected)
			return;

		std::printf("FAIL %s: observed 0x%06X, expected 0x%06X\n", _what,
			static_cast<unsigned>(_observed), static_cast<unsigned>(_expected));
		++g_failures;
	}

	/* ==================================================================
	 * Part A -- the order of one quantum, through a Scheduler.
	 * ================================================================== */

	class RecordingTrace final : public g2::TraceSink
	{
	public:
		static constexpr size_t kMax = 64;

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

	private:
		size_t         m_count = 0;
		g2::TracePhase m_phase[kMax]{};
		uint64_t       m_frame[kMax]{};
	};

	/* Records the position member of every job the Scheduler dispatches, in
	 * dispatch order, and then runs every job. */
	class RecordingExecutor final : public g2::Executor
	{
	public:
		static constexpr size_t kMaxRuns = 8;

		void run(const Job* const _jobs, const size_t _count) noexcept override
		{
			if(m_runs < kMaxRuns)
			{
				m_count[m_runs] = _count;

				for(size_t i = 0; i < _count && i < g2::kJobCount; ++i)
					m_position[m_runs][i] =
						reinterpret_cast<const g2::DspContext*>(_jobs[i].ctx)->position;
			}

			++m_runs;

			for(size_t i = 0; i < _count; ++i)
				_jobs[i].fn(_jobs[i].ctx);
		}

		bool isSerial() const noexcept override { return true; }

		size_t   runs()                                   const { return m_runs; }
		size_t   count   (const size_t r)                 const { return m_count[r]; }
		unsigned position(const size_t r, const size_t i) const { return m_position[r][i]; }

	private:
		size_t   m_runs = 0;
		size_t   m_count[kMaxRuns]{};
		unsigned m_position[kMaxRuns][g2::kJobCount]{};
	};

	const char* phaseName(const g2::TracePhase _phase)
	{
		switch(_phase)
		{
		case g2::TracePhase::Swap:    return "Swap";
		case g2::TracePhase::Ingress: return "Ingress";
		case g2::TracePhase::Panel:   return "Panel";
		case g2::TracePhase::Sof:     return "Sof";
		case g2::TracePhase::Mcu:     return "Mcu";
		case g2::TracePhase::Dsp:     return "Dsp";
		case g2::TracePhase::Egress:  return "Egress";
		}

		return "?";
	}

	/* The boot quantum's records, in order: the swap first, then the run phase,
	 * whose MCU context precedes the DSPs. */
	constexpr g2::TracePhase kBootQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp
	};

	constexpr size_t kRecordsPerQuantum = sizeof(kBootQuantum) / sizeof(kBootQuantum[0]);

	void casesThroughScheduler(g2::Board& _board)
	{
		RecordingExecutor executor;
		RecordingTrace    trace;

		g2::Scheduler::Config config;
		config.trace = &trace;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, _board, status);

		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::Ok),
			"the default Config is accepted");

		if(scheduler == nullptr)
		{
			check(false, "the default Config yields a Scheduler");
			return;
		}

		constexpr size_t kQuanta = 3;

		scheduler->runFrames(kQuanta);

		/* The swap is first and the run phase follows, on every quantum, each
		 * record carrying that quantum's frame index. */
		checkEqual(trace.count(), kQuanta * kRecordsPerQuantum,
			"one quantum emits exactly the records of the boot-regime order");

		if(trace.count() == kQuanta * kRecordsPerQuantum)
		{
			for(size_t q = 0; q < kQuanta; ++q)
			{
				for(size_t i = 0; i < kRecordsPerQuantum; ++i)
				{
					const size_t r = q * kRecordsPerQuantum + i;

					char what[256];

					std::snprintf(what, sizeof(what),
						"quantum %zu record %zu is %s", q, i, phaseName(kBootQuantum[i]));
					checkEqual(static_cast<uint64_t>(trace.phase(r)),
						static_cast<uint64_t>(kBootQuantum[i]), what);

					std::snprintf(what, sizeof(what),
						"quantum %zu record %zu carries frame index %zu", q, i, q);
					checkEqual(trace.frame(r), static_cast<uint64_t>(q), what);
				}
			}
		}

		/* The swap-before-run and MCU-before-DSP relations are derived from the
		 * expected array, not written down a second time. */
		{
			size_t swapIndex = kRecordsPerQuantum;
			size_t mcuIndex  = kRecordsPerQuantum;
			size_t dspIndex  = kRecordsPerQuantum;

			for(size_t i = 0; i < kRecordsPerQuantum; ++i)
			{
				if(kBootQuantum[i] == g2::TracePhase::Swap) swapIndex = i;
				if(kBootQuantum[i] == g2::TracePhase::Mcu)  mcuIndex  = i;
				if(kBootQuantum[i] == g2::TracePhase::Dsp)  dspIndex  = i;
			}

			checkEqual(swapIndex, 0u,
				"step 1, the swap, is the FIRST record of a quantum");
			check(mcuIndex < dspIndex,
				"step 3 runs the MCU context BEFORE DSP 0 to DSP 7");
		}

		checkEqual(executor.runs(), kQuanta,
			"the Executor is entered exactly once for each quantum");

		for(size_t q = 0; q < kQuanta && q < executor.runs(); ++q)
		{
			char what[256];

			std::snprintf(what, sizeof(what),
				"quantum %zu dispatches g2::kJobCount jobs", q);
			checkEqual(executor.count(q), g2::kJobCount, what);

			for(size_t i = 0; i < g2::kJobCount && i < executor.count(q); ++i)
			{
				std::snprintf(what, sizeof(what),
					"quantum %zu dispatch %zu is DSP position %zu (ascending, fixed order)",
					q, i, i);
				checkEqual(executor.position(q, i), static_cast<uint64_t>(i), what);
			}
		}

		/* This fixture never calls beginPlayPhase, so the Scheduler stays in the
		 * boot regime for its whole life, and a boot quantum runs the swap and
		 * the run phase alone. */
		{
			size_t codecRecords = 0;

			for(size_t r = 0; r < trace.count() && r < RecordingTrace::kMax; ++r)
				if(trace.phase(r) == g2::TracePhase::Ingress
					|| trace.phase(r) == g2::TracePhase::Egress)
					++codecRecords;

			checkEqual(codecRecords, 0u,
				"a BOOT-REGIME quantum runs neither codec edge: this fixture "
				"never calls beginPlayPhase, so the Scheduler stays in the boot "
				"regime and emits neither the ingress nor the egress record. The "
				"PLAY regime's seven records are t0_codec_regimes' to assert, and "
				"the direct adapter-level edges are PART B's");
		}
	}

	/* ==================================================================
	 * Part B -- the codec edges, against the adapter that owns them.
	 * ================================================================== */

	constexpr unsigned kPositions = 8u;
	constexpr unsigned kHopFrames = 2u;
	constexpr unsigned kDivider   = 1u;

	/* Distinct sentinels for the eight slots, so a slot that carried another
	 * slot's value is visible as a wrong value and not as a coincidence. */
	int32_t sourceSlot(const unsigned _slot) { return static_cast<int32_t>(0x210000u + _slot); }
	int32_t tailSlot  (const unsigned _slot) { return static_cast<int32_t>(0x430000u + _slot); }

	/* The value the egress destination is pre-filled with. extractCodecSink
	 * writes slots 0 and 1 and nothing else, so a surviving sentinel in slots
	 * 2..7 is the observable of "two slots, not eight". */
	constexpr int32_t kEgressSentinel = static_cast<int32_t>(0x0055AAu);

	dsp56k::Audio::TxFrame makeTailFrame(const unsigned _reg)
	{
		dsp56k::Audio::TxFrame frame;
		frame.resize(g2::Frame::kSlots);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			/* The library frame's storage is not zero initialised, so every
			 * register of every slot is written rather than only `reg`. */
			for(unsigned r = 0; r < dsp56k::Audio::TxRegisterCount; ++r)
				frame[k][r] = 0u;

			frame[k][_reg] = static_cast<dsp56k::TWord>(tailSlot(k));
		}

		return frame;
	}

	g2::Frame makeSourceFrame()
	{
		g2::Frame src{};

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			src.slot[k] = sourceSlot(k);

		return src;
	}

	/* Reads the audio receive frame position `_position` would be handed. */
	void readAudio(g2::ChainAdapter& _adapter, const unsigned _position, int32_t (&_out)[g2::Frame::kSlots])
	{
		uint64_t frameIndex = 0u;
		dsp56k::Audio::RxFrame rx;

		_adapter.audioRxCallback(_position)(frameIndex, rx);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			_out[k] = k < rx.size() ? static_cast<int32_t>(rx[k][0]) : -1;
	}

	void readSecond(g2::ChainAdapter& _adapter, const unsigned _position, int32_t (&_out)[g2::Frame::kSlots])
	{
		uint64_t frameIndex = 0u;
		dsp56k::Audio::RxFrame rx;

		_adapter.secondRxCallback(_position)(frameIndex, rx);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			_out[k] = k < rx.size() ? static_cast<int32_t>(rx[k][0]) : -1;
	}

	void fireTailTransmit(g2::ChainAdapter& _adapter, const uint64_t _frameIndex)
	{
		uint64_t frameIndex = _frameIndex;
		const dsp56k::Audio::TxFrame frame = makeTailFrame(0u);

		_adapter.audioTxCallback(kPositions - 1u)(frameIndex, frame);
	}

	void extract(g2::ChainAdapter& _adapter, g2::Frame& _out)
	{
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			_out.slot[k] = kEgressSentinel;

		_adapter.extractCodecSink(_out);
	}

	/* Position 0's receive callback reads mailbox 0's read() frame, which is
	 * the very cell ingressFrame() returns. The injected pair is therefore
	 * visible to the head in the same quantum, with no advance in between.
	 *
	 * An ingress written through write() instead of ingressFrame() puts the
	 * pair in the head's write cell, which position 0's receive does not read,
	 * and slots 0 and 1 come back 0. An ingress that copied all eight slots
	 * leaves sourceSlot(2) where silence belongs. */
	void caseIngress()
	{
		g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

		adapter.advanceAll(0u);                       /* 1. swap    */
		adapter.injectCodecSource(makeSourceFrame()); /* 2. ingress */

		int32_t head[g2::Frame::kSlots];
		readAudio(adapter, 0u, head);

		checkEqualHex(head[0], sourceSlot(0),
			"step 2: the codec source's LEFT slot reaches mailbox 0's read frame "
			"in the SAME quantum (through ingressFrame(), not write())");
		checkEqualHex(head[1], sourceSlot(1),
			"step 2: the codec source's RIGHT slot reaches mailbox 0's read frame "
			"in the SAME quantum");

		for(unsigned k = 2; k < g2::Frame::kSlots; ++k)
		{
			char what[192];
			std::snprintf(what, sizeof(what),
				"step 2 writes slots 0 and 1 ONLY: slot %u of mailbox 0's read "
				"frame stays silent", k);
			checkEqualHex(head[k], 0, what);
		}
	}

	/* advance() copies the head cell forward into the read cell before stepping
	 * the head (mailbox.cpp). An ingress performed before the swap therefore
	 * lands in the cell the swap is about to overwrite, and the pair is
	 * destroyed. Without this assertion, steps 1 and 2 could be transposed and
	 * every other case here would stay green. */
	void caseSwapPrecedesIngress()
	{
		g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

		adapter.injectCodecSource(makeSourceFrame());   /* 2 before 1: WRONG order */
		adapter.advanceAll(0u);

		int32_t head[g2::Frame::kSlots];
		readAudio(adapter, 0u, head);

		checkEqualHex(head[0], 0,
			"an ingress performed BEFORE the swap is destroyed by it, so the "
			"row's order (swap, then ingress) is load-bearing");
		checkEqualHex(head[1], 0,
			"an ingress performed BEFORE the swap is destroyed by it (right slot)");
	}

	/* The tail position writes mailbox N through its transmit callback, and
	 * egressFrame() is that mailbox's write cell, so the tail's frame reaches
	 * the codec sink in the same quantum.
	 *
	 * An egress read through read() instead of egressFrame() reads the cell one
	 * past the head, which the tail has not written, and reports 0. An egress
	 * read from m_audio.front() instead of back() reads mailbox 0, which no
	 * transmit callback ever writes, and reports 0. An egress that copied all
	 * eight slots overwrites the destination's slots 2..7. */
	void caseEgress()
	{
		g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

		adapter.advanceAll(0u);         /* 1. swap */
		fireTailTransmit(adapter, 0u);  /* 3. run  */

		g2::Frame out;
		extract(adapter, out);          /* 4. egress */

		checkEqualHex(out.slot[0], tailSlot(0),
			"step 4: the tail's LEFT slot reaches the codec sink in the SAME "
			"quantum (through egressFrame(), not read())");
		checkEqualHex(out.slot[1], tailSlot(1),
			"step 4: the tail's RIGHT slot reaches the codec sink in the SAME "
			"quantum");

		for(unsigned k = 2; k < g2::Frame::kSlots; ++k)
		{
			char what[192];
			std::snprintf(what, sizeof(what),
				"step 4 reads slots 0 and 1 ONLY: slot %u of the destination is "
				"left untouched", k);
			checkEqualHex(out.slot[k], kEgressSentinel, what);
		}
	}

	/* An egress taken before the run phase reports the previous quantum's
	 * frame. A pair of tail frames that were equal would prove nothing, so
	 * their inequality is asserted too. */
	void caseEgressFollowsRun()
	{
		g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

		/* A tail frame of zeroes is what a fresh adapter already holds, so the
		 * first quantum's tail frame is the sentinel-bearing one and the second
		 * quantum's is silence. */
		adapter.advanceAll(0u);
		fireTailTransmit(adapter, 0u);

		g2::Frame first;
		extract(adapter, first);

		adapter.advanceAll(1u);

		g2::Frame beforeRun;
		extract(adapter, beforeRun);

		{
			uint64_t frameIndex = 1u;
			dsp56k::Audio::TxFrame silence;
			silence.resize(g2::Frame::kSlots);
			for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
				for(unsigned r = 0; r < dsp56k::Audio::TxRegisterCount; ++r)
					silence[k][r] = 0u;

			adapter.audioTxCallback(kPositions - 1u)(frameIndex, silence);
		}

		g2::Frame afterRun;
		extract(adapter, afterRun);

		check(first.slot[0] != afterRun.slot[0],
			"the two quanta's tail frames DIFFER (a pair that agreed would make "
			"the ordering assertion below vacuous)");
		checkEqualHex(beforeRun.slot[0], first.slot[0],
			"an egress taken BEFORE the run phase reports the PREVIOUS quantum's "
			"tail frame, so step 4 must follow step 3");
		checkEqualHex(afterRun.slot[0], 0,
			"an egress taken AFTER the run phase reports THIS quantum's tail frame");
	}

	/* The second bus is the only bus whose topology can be a Ring, and the two
	 * codec edges are audio-bus-only. */
	void caseRingHasNoCodecEdge()
	{
		{
			g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

			adapter.advanceAll(0u);
			adapter.injectCodecSource(makeSourceFrame());

			for(unsigned p = 0; p < kPositions; ++p)
			{
				int32_t got[g2::Frame::kSlots];
				readSecond(adapter, p, got);

				for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
				{
					char what[192];
					std::snprintf(what, sizeof(what),
						"the ingress does not run for a Ring: second-bus mailbox "
						"%u slot %u stays silent", p, k);
					checkEqualHex(got[k], 0, what);
				}
			}
		}

		{
			g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

			adapter.advanceAll(0u);

			/* Every position transmits on the second bus, so every second-bus
			 * mailbox of the ring carries a sentinel. */
			for(unsigned p = 0; p < kPositions; ++p)
			{
				uint64_t frameIndex = 0u;
				const dsp56k::Audio::TxFrame frame = makeTailFrame(2u);
				adapter.secondTxCallback(p)(frameIndex, frame);
			}

			g2::Frame out;
			extract(adapter, out);

			checkEqualHex(out.slot[0], 0,
				"the egress does not run for a Ring: a second-bus transmit does "
				"not reach the codec sink (left slot)");
			checkEqualHex(out.slot[1], 0,
				"the egress does not run for a Ring: a second-bus transmit does "
				"not reach the codec sink (right slot)");
		}
	}

	/* A frame written into a mailbox's write cell reaches its read cell after
	 * exactly hopFrames advances (mailbox.cpp's index relation). One quantum
	 * performs exactly one advance, so the frame arrives on the hopFrames-th
	 * quantum after the one it was written in, not earlier. An advanceAll that
	 * swapped the audio bus twice, or a second swap point anywhere else in the
	 * adapter, delivers the frame early and the "not yet" half goes red. */
	void caseSingleSwapPoint()
	{
		g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

		const unsigned hop = adapter.hopFrames();

		check(hop >= 2u,
			"the fixture's hop is at least 2, so there is a quantum on which the "
			"frame has NOT yet arrived (a hop of 1 has no such quantum)");

		adapter.advanceAll(0u);

		{
			uint64_t frameIndex = 0u;
			const dsp56k::Audio::TxFrame frame = makeTailFrame(0u);
			adapter.audioTxCallback(0u)(frameIndex, frame);   /* writes mailbox 1 */
		}

		for(unsigned k = 1; k <= hop; ++k)
		{
			adapter.advanceAll(k);

			int32_t got[g2::Frame::kSlots];
			readAudio(adapter, 1u, got);   /* position 1 reads mailbox 1 */

			char what[224];

			if(k < hop)
			{
				std::snprintf(what, sizeof(what),
					"one quantum performs exactly ONE swap: after %u of %u "
					"advances position 0's frame has NOT reached position 1",
					k, hop);
				checkEqualHex(got[0], 0, what);
			}
			else
			{
				std::snprintf(what, sizeof(what),
					"one quantum performs exactly ONE swap: after %u of %u "
					"advances position 0's frame HAS reached position 1", k, hop);
				checkEqualHex(got[0], tailSlot(0), what);
			}
		}
	}

	/* The swap advances a second-bus mailbox only when
	 * frameIndex % secondBusFrameDivider == 0. A frame written into a
	 * second-bus mailbox at a window quantum therefore reaches its read cell
	 * after hopFrames window advances, which is hopFrames *
	 * secondBusFrameDivider quanta and not hopFrames quanta.
	 *
	 * A swap that advanced the second bus on every quantum delivers the frame
	 * at quantum hopFrames and every "not yet" assertion before it goes red. */
	void caseSecondBusAdvancesOnlyInItsWindow()
	{
		constexpr unsigned kWindowDivider = 4u;

		g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kWindowDivider);

		const unsigned hop     = adapter.hopFrames();
		const unsigned divider = adapter.secondBusFrameDivider();

		check(divider >= 2u,
			"the fixture's second-bus divider is at least 2, so a non-window "
			"quantum exists (at a divider of 1 every quantum is a window and "
			"the gate has nothing to discriminate)");

		adapter.advanceAll(0u);   /* frame index 0 is a window at every divider */

		{
			uint64_t frameIndex = 0u;
			const dsp56k::Audio::TxFrame frame = makeTailFrame(2u);
			adapter.secondTxCallback(0u)(frameIndex, frame);   /* writes second mailbox 1 */
		}

		const unsigned arrival = hop * divider;

		for(unsigned q = 1; q <= arrival; ++q)
		{
			adapter.advanceAll(q);

			int32_t got[g2::Frame::kSlots];
			readSecond(adapter, 1u, got);

			char what[224];

			if(q < arrival)
			{
				std::snprintf(what, sizeof(what),
					"the second bus advances only in its window: at quantum %u of "
					"hop %u x divider %u the frame has NOT reached position 1",
					q, hop, divider);
				checkEqualHex(got[0], 0, what);
			}
			else
			{
				std::snprintf(what, sizeof(what),
					"the second bus advances only in its window: at quantum %u, "
					"hop %u x divider %u, the frame HAS reached position 1",
					q, hop, divider);
				checkEqualHex(got[0], tailSlot(0), what);
			}
		}
	}
}

int main()
{
	std::printf("t0_four_phase: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	/* These need no Scheduler, no Board and no backend: the chain adapter is a
	 * plain object and the codec edges are its own members. They run in every
	 * build. */
	caseIngress();
	caseSwapPrecedesIngress();
	caseEgress();
	caseEgressFollowsRun();
	caseRingHasNoCodecEdge();
	caseSingleSwapPoint();
	caseSecondBusAdvancesOnlyInItsWindow();

	/* An interpreter build yields no Scheduler at all, so asserting the refusal
	 * is the only claim this file may make in such a build.
	 *
	 * The Board is declared before every Scheduler: the Board outlives the
	 * Scheduler. */
	{
		g2::Board board;

		if(!dsp56k::g_useJIT)
		{
			RecordingExecutor executor;
			g2::Scheduler::Config config;
			g2::Status status{};

			const std::unique_ptr<g2::Scheduler> scheduler =
				g2::Scheduler::create(config, executor, board, status);

			check(scheduler == nullptr, "an interpreter build yields no Scheduler");
			checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::BadBackend),
				"an interpreter build reports BadBackend");
		}
		else
		{
			casesThroughScheduler(board);
		}
	}

	if(g_failures != 0)
	{
		std::printf("t0_four_phase: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_four_phase: all %d cases passed\n", g_cases);
	return 0;
}
