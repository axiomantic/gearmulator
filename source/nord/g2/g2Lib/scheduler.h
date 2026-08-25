/* scheduler.h -- the G2 scheduler's header.
 *
 * In an interpreter build no `Scheduler` can be created at all, which is the
 * correct outcome because `runDspCycles` cannot terminate in such a build (the
 * DSP's `m_cycles` counter is never written). The `Interpreter` enumerator
 * stays because the semantic cross-check harness drives `DSP::exec` directly
 * and never constructs a `Scheduler`.
 *
 * `Config::trace` carries a `TraceSink`. A null sink records nothing and is the
 * default, so a production Scheduler pays one null check for each phase of each
 * quantum. It is production surface whose only consumer is a check, and it is
 * here because the swap, the panel, the start-of-frame tick and the MCU all run
 * SERIALLY in the Scheduler, outside the Executor -- so the order they run in
 * has no other decider. The alternative was four accessors, one for each thing
 * a check must reach, and four pieces of surface that only move the wall are a
 * worse object than one seam that makes a real mutation go red.
 *
 * `g_useJIT` is a `static constexpr` at dsp.h:36, so the branch that reads it
 * folds at compile time and the interpreter build pays nothing for it. One
 * build carries one backend and the property is structural with no run-time
 * observable.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <thread>

#include "chainAdapter.h"
#include "codecQueues.h"
#include "executor.h"
#include "mcuContext.h"
#include "status.h"

#include "g2/timebase.h"

#include "dsp56kEmu/dsp.h"

namespace g2
{
	class Board;

	/* The backend. Fixed for the whole BINARY, by dsp56300's own
	 * `static constexpr bool g_useJIT` at dsp.h:36. This enum therefore
	 * records which backend the binary was built with; it does not select
	 * one. §11.4.1.
	 *
	 * ONE RULE GOVERNS `Config::backend`, §11.4.3: create() succeeds only
	 * when backend == Backend::Jit AND `g_useJIT` is true. Any other
	 * combination returns a null Scheduler object. */
	enum class Backend { Jit, Interpreter };

	/* The phase tags of one quantum, in order. */
	enum class TracePhase : uint32_t
	{
		Swap, Ingress, Panel, Sof, Mcu, Dsp, Egress
	};

	class TraceSink
	{
	public:
		virtual ~TraceSink() = default;

		virtual void onPhase(TracePhase _phase, uint64_t _frameIndex) noexcept = 0;
	};

	class Scheduler
	{
	public:
		/* Every default is a legal value, so a caller that perturbs one field
		 * is rejected for that field and for nothing else. Where a shipped
		 * constant exists the default IS that constant; `lookaheadFrames` and
		 * `maxHostBlockFrames` have none, so those two carry the smallest
		 * legal value instead. That is all the defaults claim: they are not
		 * the shipped machine's configuration and no caller may read them as
		 * one. */
		struct Config
		{
			unsigned      dspCount              = 8;
			unsigned      framesPerQuantum      = 1;
			unsigned      lookaheadFrames       = 1;
			unsigned      maxHostBlockFrames    = 1;
			unsigned      hopFrames             = G2_CHAIN_HOP_FRAMES;
			ChainTopology secondBusTopology     = ChainTopology::Ring;
			unsigned      secondBusFrameDivider = G2_SECOND_BUS_FRAME_DIVIDER;
			Backend       backend               = Backend::Jit;

			/* Both rationals reach create() and both are checked there. The
			 * check is NOT inside alloc(), which runs once for each context
			 * for each quantum and must carry no branch that cannot fire. */
			Rational      dspRate               = { G2_DSP_CYCLES_PER_FRAME_NUM, G2_DSP_CYCLES_PER_FRAME_DEN };
			Rational      mcuRate               = { G2_MCU_CYCLES_PER_FRAME_NUM, G2_MCU_CYCLES_PER_FRAME_DEN };

			/* The escape hatch from the two equality rows, and nothing else: a
			 * strict equality against the build constants would forbid tests at
			 * H = 1 and H = 2 and at divider 1 and 4.
			 *
			 * It does not reach the zero rows. A hop of zero collapses the hop
			 * and a divider of zero is a zero divisor, and neither becomes
			 * legal because a test asked for it.
			 *
			 * False by default, so the escape is taken only by a caller that
			 * names it. The golden-render path asserts it is false, so the one
			 * path that records a reference cannot take it. */
			bool          testOverride          = false;

			/* NULL means no recording, and it is the default. */
			TraceSink*    trace                 = nullptr;
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

		/* The quantum entry point, and the only way a quantum happens. It turns
		 * `_frames` whole quanta.
		 *
		 * `void` and `noexcept`: the Device takes a fault AFTER this returns,
		 * through the Scheduler's own fault surface. A return value carrying
		 * the fault would give that property a second home.
		 *
		 * A frame count and not a quantum count. `framesPerQuantum` is fixed at
		 * 1 and every calling site counts frames.
		 *
		 * The owning thread is the caller's and this object creates none. The
		 * serial executor runs the jobs on this same thread; a parallel one may
		 * use workers inside run(), which returns only when every job has. */
		void runFrames(size_t _frames) noexcept;

		/* THE BOOT-TO-PLAY TRANSITION, AS ONE CALL, and the boot thread's last
		 * Scheduler action. Design section 13.10 rule 3 states its five steps
		 * and scheduler.cpp carries them in order.
		 *
		 * IT LEAVES THE PLAY PHASE'S INITIAL CONDITION AND NOTHING ELSE: the
		 * CodecSource empty, the CodecSink holding exactly `lookaheadFrames`
		 * frames, all seven chain-health counters zero and the recorded
		 * owning-thread identity cleared. It touches no emulated machine
		 * state, clears no fault, and does not reset the frame index -- its own
		 * `lookaheadFrames` quanta advance it by that much.
		 *
		 * `noexcept`, matching runFrames, because design section 13.10 rule 2
		 * forbids throwing. */
		void beginPlayPhase() noexcept;

		/* HOST INPUT. Returns the number of frames the CodecSource ACCEPTED.
		 * A return below `_frames` means the queue refused the remainder,
		 * which the capacity rule of design section 13.6.1 makes unreachable
		 * in a correct build -- so a short return is a DEFECT REPORT and
		 * overflowFrames() is how it is counted. */
		size_t push(const Frame* _in, size_t _frames) noexcept;

		/* HOST OUTPUT. Returns the frames actually taken. The part the sink
		 * could not supply reads as SILENCE and raises underflowFrames by the
		 * shortfall. */
		size_t pull(Frame* _out, size_t _frames) noexcept;

		/* THE VIRTUAL CLOCK. It advances once for each quantum and nothing
		 * else moves it. */
		uint64_t frameIndex() const noexcept;

		/* THE SEVEN CHAIN-HEALTH COUNTERS of design section 13.10.5's
		 * observability block. None of them influences emulation and all
		 * seven are zeroed by beginPlayPhase.
		 *
		 * THE FIRST THREE READ THROUGH A BASELINE THIS OBJECT KEEPS, and the
		 * reason is stated rather than hidden: they live on the ChainAdapter,
		 * which CHN-5, CHN-7 and CHN-8 own and which exposes no way to zero
		 * them. Zeroing the ADAPTER is not an option either -- its mailboxes
		 * are emulated state and beginPlayPhase may not touch that. So the
		 * SCHEDULER's counter, which is the one design section 13.10.5
		 * declares, is the adapter's reading minus the reading taken at the
		 * last zeroing. */
		uint64_t underrunFrames(unsigned _position) const noexcept;
		uint64_t secondBusUnderrunFrames(unsigned _position) const noexcept;
		uint64_t phaseErrorFrames(unsigned _position) const noexcept;
		uint64_t starvedFrames() const noexcept;
		uint64_t overflowFrames() const noexcept;
		uint64_t droppedFrames() const noexcept;
		uint64_t underflowFrames() const noexcept;

		/* THE TWO DIAGNOSTIC COUNTERS. Both are emulated-cycle quantities and
		 * NEITHER MEASURES HOST TIME. A non-zero value is a finding, not
		 * necessarily a failure, which is why these two are not among the
		 * seven asserted zero.
		 *
		 * THE CONTEXT INDEX IS DESIGN SECTION 13.5's: 0 is the MCU and
		 * 1 .. dspCount are the DSPs, ascending. An index above dspCount
		 * reads back zero rather than running off the end. */
		int64_t  cycleDebt(unsigned _contextIndex) const noexcept;
		uint64_t longDispatchQuanta(unsigned _contextIndex) const noexcept;

		/* THE FAULT SURFACE, design section 13.10.5. A fault is STICKY: once a
		 * context has faulted, nothing but reset() clears it, and the faulted
		 * context is never dispatched again.
		 *
		 * `faulted()` IS THE DISJUNCTION over every context and is what the
		 * Device reads AFTER runFrames returns. THE CONTEXT INDEX IS DESIGN
		 * SECTION 13.5's: 0 is the MCU and 1 .. dspCount are the DSPs,
		 * ascending -- the same index cycleDebt and longDispatchQuanta take. An
		 * index above dspCount reads back false and JobFault::None rather than
		 * running off the end.
		 *
		 * NO EXCEPTION AND NO ASSERTION IS INVOLVED. Design section 13.10 rule
		 * 2 forbids throwing and a release build removes an assertion, so a
		 * fault is observable through these three and through nothing else. */
		bool     faulted() const noexcept;
		bool     contextFaulted(unsigned _contextIndex) const noexcept;
		JobFault contextFault(unsigned _contextIndex) const noexcept;

		/* THE RESET, design section 13.10.5, and the boot thread's call. It
		 * returns this object and the machine it drives to the state a freshly
		 * created Scheduler is in: every fault cleared and every context back
		 * in the dispatch set, the virtual clock at frame 0, the boot regime,
		 * both codec queues empty, every counter and every debt zero, the
		 * recorded owning thread cleared, and the emulated memories the objects
		 * below own zeroed.
		 *
		 * WHAT IT DOES NOT ZERO IS STATED IN board.cpp AT Board::reset, because
		 * the limit is that object's and not this one's. */
		void reset() noexcept;

		/* THE RECORDED OWNING THREAD. runFrames records the calling thread on
		 * its first call after each clearing, and beginPlayPhase's step 5
		 * clears the record so the first runFrames of the play phase
		 * re-establishes it on the audio thread.
		 *
		 * IT IS EXPOSED BECAUSE THE PROPERTY MUST BE CHECKABLE IN A RELEASE
		 * BUILD. A debug assertion is removed by NDEBUG, so a check whose
		 * predicate was the assertion would check the wrong build. A
		 * default-constructed id means NO OWNER IS RECORDED. */
		std::thread::id owningThread() const noexcept;

		/* A Scheduler is neither copied nor moved. It installs callbacks that
		 * capture `this` into ESAIs the Board owns, and those callbacks are
		 * not re-pointed by a copy or a move -- so a second object would hold
		 * a ChainAdapter nothing is wired to. The static_asserts below the
		 * class are what keep this true. */
		Scheduler(const Scheduler&)            = delete;
		Scheduler& operator=(const Scheduler&) = delete;
		Scheduler(Scheduler&&)                 = delete;
		Scheduler& operator=(Scheduler&&)      = delete;

	private:
		/* Private: `create` is the only caller and it runs the whole rejection
		 * table first, so every value read below has already been vetted. */
		Scheduler(const Config& _config, Executor& _executor, Board& _board);

		/* One trace record, or nothing at all when no sink was supplied. */
		void mark(TracePhase _phase, uint64_t _frameIndex) const noexcept;

		/* Both are borrowed and neither is owned, and both must OUTLIVE this
		 * object. The Board's rule is the sharper of the two: every context
		 * below borrows a core, two ESAI ports and a landed flag owned by a
		 * slot of the Board's DSP set, and the chain callbacks this object
		 * installs sit on that set's ESAIs. A Board destroyed first leaves all
		 * of it dangling AT TEARDOWN, which is what makes it easy to miss.
		 * Declaration order at each call site is what enforces it -- a
		 * Scheduler declared after its Board is destroyed before it -- and the
		 * factory says in as many words that it cannot check the lifetime of a
		 * referent. */
		Executor&  m_executor;
		Board&     m_board;

		/* NULL means no recording, so a production Scheduler pays one null
		 * check for each phase of each quantum and nothing else. */
		TraceSink* m_trace;

		/* Held by value: the Scheduler owns exactly one ChainAdapter.
		 *
		 * The callbacks it hands to the Board's ESAIs outlive it, and this
		 * object installs no uninstaller. A destructor that cleared them would
		 * be new lifetime machinery in a file whose members' declaration order
		 * is already load-bearing, so the rule is the same one the Board obeys
		 * internally: a Scheduler is destroyed before the Board it was given,
		 * and nothing drives an ESAI register between one Scheduler's death and
		 * the next one's install. */
		ChainAdapter m_chain;

		/* THE MCU CONTEXT, CONTEXT INDEX 0. It carries the rate, the rational
		 * accumulator, the cycle debt and the rule 4 counter, and the ONE
		 * block of design section 13.4.6 -- g2::runQuantum -- wraps
		 * Board::runMcu with it. It is not a JobContext and it does not enter
		 * the job array, which stays at exactly kJobCount. */
		McuContext m_mcu;

		/* The authoritative virtual frame index. Every phase of one quantum
		 * reads the SAME value, and it advances once for each quantum. */
		uint64_t m_frameIndex = 0;

		/* THE CODEC REGIME. The object knows its phase and the CALLER DOES NOT
		 * SELECT IT: a Scheduler is born in Boot and beginPlayPhase is the one
		 * thing that moves it to Play. In Boot a quantum runs the swap and the
		 * run phase only -- no ingress, no egress, neither queue touched. In
		 * Play all four phases run. Without the boot regime the sink fills
		 * after L + B boot quanta and the scheduler stops part-way through the
		 * boot, which is the defect this regime closes. */
		enum class CodecRegime { Boot, Play };

		CodecRegime m_regime = CodecRegime::Boot;

		/* THE TWO CODEC QUEUES, OWNED HERE. Design section 13.10.5 makes the
		 * Scheduler the owner of both. Both capacities are
		 * lookaheadFrames + maxHostBlockFrames, and both rings are allocated
		 * once, here, and never resized by a quantum. */
		CodecSource m_source;
		CodecSink   m_sink;

		/* L, KEPT BECAUSE beginPlayPhase NEEDS IT TWICE -- once to prime and
		 * once to count its own quanta -- and the Config is not stored. */
		unsigned m_lookaheadFrames;
		size_t   m_codecCapacity;

		/* THE BASELINE OF THE THREE ADAPTER-OWNED COUNTERS, one for each
		 * position for each of the three. See the accessor comments above:
		 * the ChainAdapter cannot be zeroed without destroying emulated
		 * state, so the Scheduler subtracts the reading it took at the last
		 * zeroing. */
		std::vector<uint64_t> m_underrunBase;
		std::vector<uint64_t> m_secondUnderrunBase;
		std::vector<uint64_t> m_phaseErrorBase;

		/* THE RECORDED OWNING THREAD. A default-constructed id means no owner
		 * is recorded, which is the state beginPlayPhase's step 5 leaves and
		 * the state a fresh Scheduler starts in. */
		std::thread::id m_owner{};

		/* THE STICKY FAULT LATCH, ONE ENTRY FOR EACH CONTEXT INDEX, in design
		 * section 13.5's numbering: entry 0 is the MCU and 1 .. kJobCount are
		 * the DSPs. It is the SCHEDULER's record and not the job's: a job's own
		 * `base.fault` is overwritten by the next quantum that dispatches it,
		 * and the MCU has no fault field at all -- Board::faulted() is one bit
		 * with no code beside it. Latching here is what makes contextFault
		 * answer for EVERY index rather than for the DSPs only. */
		JobFault m_fault[1u + kJobCount]{};

		/* THE DISJUNCTION OVER THE LATCH, KEPT RATHER THAN COMPUTED. It is what
		 * the Device reads after every runFrames, and a scan of nine entries on
		 * that path buys nothing. */
		bool m_faulted = false;

		/* THE JOB ARRAY, BUILT ONCE. `Job::ctx` points at each context's
		 * LEADING JobContext, which dspContext.h's two static_asserts are what
		 * make legal. */
		DspContext    m_contexts[kJobCount]{};
		Executor::Job m_jobs[kJobCount]{};

		/* THE DISPATCH SET, AND IT IS A SECOND ARRAY RATHER THAN A COUNT INTO
		 * THE FIRST. Executor::run takes a CONTIGUOUS array and a count, so a
		 * faulted context in the middle cannot be skipped by lowering the
		 * count; the live jobs are compacted into this array instead, and
		 * m_jobs keeps its position-indexed order so that nothing else has to
		 * learn about the compaction.
		 *
		 * A FAULTED CONTEXT IS NEVER DISPATCHED AGAIN, which is the property
		 * this array exists to deliver, and reset() is the only thing that puts
		 * one back. */
		Executor::Job m_liveJobs[kJobCount]{};
		size_t        m_liveCount = 0;

		/* Rebuilds m_liveJobs from m_fault. Called at construction, whenever a
		 * fault latches, and by reset(). */
		void rebuildDispatchSet() noexcept;

		/* Reads the fault of every context back after the run phase and latches
		 * whatever it finds. Returns true when at least one NEW fault latched,
		 * which is when the dispatch set has to be rebuilt. */
		bool latchFaults() noexcept;
	};

	/* Neither copyable nor movable, as a compile-time property so that it
	 * cannot be silently lost. The hazard is specific: a copy would duplicate
	 * the by-value ChainAdapter while the Board's ESAIs stay bound to the
	 * ORIGINAL's callbacks, so the copy would run quanta against mailboxes
	 * nothing reads -- silently dead, with no diagnostic anywhere. The two
	 * reference members already suppress assignment; they do not suppress
	 * construction, which is why the four are deleted explicitly. */
	static_assert(!std::is_copy_constructible_v<Scheduler>,
	              "Scheduler must not be copy constructible");
	static_assert(!std::is_copy_assignable_v<Scheduler>,
	              "Scheduler must not be copy assignable");
	static_assert(!std::is_move_constructible_v<Scheduler>,
	              "Scheduler must not be move constructible");
	static_assert(!std::is_move_assignable_v<Scheduler>,
	              "Scheduler must not be move assignable");
}
