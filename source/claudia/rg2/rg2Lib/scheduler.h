#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include <thread>

#include "chainAdapter.h"
#include "codecQueues.h"
#include "executor.h"
#include "mcuContext.h"
#include "status.h"

#include "rg2/timebase.h"

#include "dsp56kEmu/dsp.h"

namespace rg2
{
	class Board;

	enum class Backend
	{
		Jit,
		Interpreter
	};

	enum class TracePhase : uint32_t
	{
		Swap,
		Ingress,
		Panel,
		Sof,
		Mcu,
		Dsp,
		Egress
	};

	class TraceSink
	{
	public:
		virtual ~TraceSink() = default;
		virtual void onPhase(TracePhase _phase, uint64_t _frameIndex) noexcept = 0;
	};

	// Optional MCU quantum execution hook (used by debuggers to intercept execution)
	class McuRunner
	{
	public:
		virtual ~McuRunner() = default;
		virtual uint32_t runMcu(uint32_t _want) noexcept = 0;
	};

	// Quantum scheduler coordinating MCU, DSP set, and inter-processor transport
	class Scheduler
	{
	public:
		struct Config
		{
			unsigned dspCount = 8;
			unsigned framesPerQuantum = 1;
			unsigned lookaheadFrames = 1;
			unsigned maxHostBlockFrames = 1;
			unsigned hopFrames = G2_CHAIN_HOP_FRAMES;
			ChainTopology secondBusTopology = ChainTopology::Ring;
			unsigned secondBusFrameDivider = G2_SECOND_BUS_FRAME_DIVIDER;
			Backend backend = Backend::Jit;

			Rational dspRate = {G2_DSP_CYCLES_PER_FRAME_NUM, G2_DSP_CYCLES_PER_FRAME_DEN};
			Rational mcuRate = {G2_MCU_CYCLES_PER_FRAME_NUM, G2_MCU_CYCLES_PER_FRAME_DEN};

			// Bypasses strict equality checks for non-standard test configurations
			bool testOverride = false;

			// Explicit DSP chain ordering (empty = dynamically derived from firmware boot table)
			std::vector<unsigned> chainOrder{};

			TraceSink* trace = nullptr;
		};

		/* The factory, and the single rejection point.
		 *
		 * `_outStatus` is written on every path, including the successful one.
		 * A null return with `Status::Unset` left in place would be a path that
		 * rejected without saying why.
		 *
		 * No exception and no assertion: a release build removes an assertion,
		 * so the rejections are observable there through the return value and
		 * the status and through nothing else. */
		static std::unique_ptr<Scheduler> create(const Config& _config, Executor& _executor, Board& _board,
												 Status& _outStatus);

		// Drives _frames quanta through the execution pipeline
		void runFrames(size_t _frames) noexcept;

		void setMcuRunner(McuRunner* _runner) noexcept { m_mcuRunner = _runner; }
		McuRunner* mcuRunner() const noexcept { return m_mcuRunner; }

		// Transitions from boot regime to play regime, initializing codec queues
		void beginPlayPhase() noexcept;

		// Host audio I/O
		size_t push(const Frame* _in, size_t _frames) noexcept;
		size_t pull(Frame* _out, size_t _frames) noexcept;

		uint64_t frameIndex() const noexcept;

		// Chain transport and queue health counters (zeroed at beginPlayPhase)
		uint64_t underrunFrames(unsigned _position) const noexcept;
		uint64_t secondBusUnderrunFrames(unsigned _position) const noexcept;
		uint64_t phaseErrorFrames(unsigned _position) const noexcept;
		uint64_t starvedFrames() const noexcept;
		uint64_t overflowFrames() const noexcept;
		uint64_t droppedFrames() const noexcept;
		uint64_t underflowFrames() const noexcept;

		// Emulated cycle metrics (context 0 = MCU, 1..dspCount = DSPs)
		int64_t cycleDebt(unsigned _contextIndex) const noexcept;
		uint64_t longDispatchQuanta(unsigned _contextIndex) const noexcept;

		// Sticky fault inspection
		bool faulted() const noexcept;
		bool contextFaulted(unsigned _contextIndex) const noexcept;
		JobFault contextFault(unsigned _contextIndex) const noexcept;

		size_t stateSize() const noexcept;
		void stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		void reset() noexcept;

		bool chainAttached() const noexcept;
		std::thread::id owningThread() const noexcept;

		Scheduler(const Scheduler&) = delete;
		Scheduler& operator=(const Scheduler&) = delete;
		Scheduler(Scheduler&&) = delete;
		Scheduler& operator=(Scheduler&&) = delete;

	private:
		Scheduler(const Config& _config, Executor& _executor, Board& _board);

		void mark(TracePhase _phase, uint64_t _frameIndex) const noexcept;

		Executor& m_executor;
		Board& m_board;
		TraceSink* m_trace;
		McuRunner* m_mcuRunner = nullptr;

		ChainAdapter m_chain;
		McuContext m_mcu;
		uint64_t m_frameIndex = 0;

		enum class CodecRegime
		{
			Boot,
			Play
		};
		CodecRegime m_regime = CodecRegime::Boot;

		CodecSource m_source;
		CodecSink m_sink;

		unsigned m_lookaheadFrames;
		size_t m_codecCapacity;

		std::vector<uint64_t> m_underrunBase;
		std::vector<uint64_t> m_secondUnderrunBase;
		std::vector<uint64_t> m_phaseErrorBase;

		std::thread::id m_owner{};

		JobFault m_fault[1u + kJobCount]{};
		bool m_faulted = false;

		DspContext m_contexts[kJobCount]{};
		Executor::Job m_jobs[kJobCount]{};

		// Compacted dispatch array of non-faulted jobs
		Executor::Job m_liveJobs[kJobCount]{};
		size_t m_liveCount = 0;

		void rebuildDispatchSet() noexcept;
		void attachChainIfOrderKnown() noexcept;

		std::vector<unsigned> m_chainOrder;
		bool m_chainAttached = false;
		std::vector<unsigned> m_configuredChainOrder;

		bool latchFaults() noexcept;
	};

	/* Neither copyable nor movable, as a compile-time property so that it
	 * cannot be silently lost. The hazard is specific: a copy would duplicate
	 * the by-value ChainAdapter while the Board's ESAIs stay bound to the
	 * ORIGINAL's callbacks, so the copy would run quanta against mailboxes
	 * nothing reads -- silently dead, with no diagnostic anywhere. The two
	 * reference members already suppress assignment; they do not suppress
	 * construction, which is why the four are deleted explicitly. */
	static_assert(!std::is_copy_constructible_v<Scheduler>, "Scheduler must not be copy constructible");
	static_assert(!std::is_copy_assignable_v<Scheduler>, "Scheduler must not be copy assignable");
	static_assert(!std::is_move_constructible_v<Scheduler>, "Scheduler must not be move constructible");
	static_assert(!std::is_move_assignable_v<Scheduler>, "Scheduler must not be move assignable");
} // namespace rg2
