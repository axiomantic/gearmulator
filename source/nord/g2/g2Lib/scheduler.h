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
 * 1. The `Backend` enum, SCH-17's. It records which backend the binary was
 *    built with; it does not select one.
 *
 * 2. The `Scheduler::Config` struct. Every rejectable value of design section
 *    13.10.5 arrives through it, which is what makes the factory the single
 *    rejection point.
 *
 * 3. The `Scheduler::create` factory, DECLARED and not defined.
 *
 * 4. SCH-19's `TracePhase` and `TraceSink`, and the `Config::trace` member
 *    that carries one. A null sink records nothing and is the default, so a
 *    production Scheduler pays one null check for each phase of each quantum.
 *    IT IS PRODUCTION SURFACE WHOSE ONLY CONSUMER IS A CHECK, and it is here
 *    because the swap, the panel, the start-of-frame tick and the MCU all run
 *    SERIALLY in the Scheduler, outside the Executor -- so the order they run
 *    in has no other decider. Those phases and the Executor dispatch are the
 *    whole of a quantum here: the ingress and the egress are PLAY REGIME ONLY,
 *    this class carries no regime member, and SCH-22 is what adds them and
 *    their two records. The alternative was four accessors, one for each thing
 *    a check must reach, and four pieces of surface that only move the wall are
 *    a worse object than one seam that makes a real mutation go red.
 *
 * 5. SCH-19's `runFrames`, the quantum entry point, and the private
 *    constructor that wires the Executor and the Board in.
 *
 * 6. `McuRunner` and `setMcuRunner`. A null runner means the MCU phase of a
 *    quantum calls `Board::runMcu`, so a production Scheduler pays one further
 *    null check for each quantum. It exists because a debugger needs a
 *    decision point between two MCU instructions and a quantum offers none;
 *    a debugger that carried its own copy of the quantum order instead would
 *    be a second full-advance path that nothing keeps in step with this one.
 *
 * THE RULE SCH-17 OWNS, STATED IN ONE SENTENCE.
 *
 *   `Scheduler::create` succeeds only when `config.backend == Backend::Jit`
 *    AND `dsp56k::g_useJIT` is true. Any other combination returns a null
 *    `Scheduler` object.
 *
 * Design section 11.4.3 states the rule and gives the consequences: in
 * a JIT build only `Backend::Jit` is accepted; in an interpreter build no
 * `Scheduler` can be created at all, which is the correct outcome because
 * `runDspCycles` cannot terminate in such a build (the DSP's `m_cycles`
 * counter is never written); and the `Interpreter` enumerator stays because
 * the semantic cross-check harness of design section 11.4.3 drives
 * `DSP::exec` directly and never constructs a `Scheduler`.
 *
 * WHY THE IMPLEMENTATION IS NOT INLINE.
 *
 * The alternative was an inline body, and it was the right answer while the
 * factory was one branch: a header that grows through SCH-18, SCH-19, SCH-21,
 * SCH-22, SCH-23, SCH-24, SCH-28 and SCH-30 has no business owning a
 * translation unit while its whole content is a single comparison. That
 * stopped being true at SCH-18, which lands the rejection table. The body is
 * now in `scheduler.cpp`, and SCH-18 opens that file rather than SCH-19,
 * because §7.4.2 gives a path to the FIRST WRITER IN THE DEPENDS CHAIN and
 * SCH-19 is not inside SCH-18's closure -- the edge runs the other way.
 *
 * `g_useJIT` IS A `static constexpr` AT dsp.h:36, so the branch that reads it
 * folds at compile time and the interpreter build pays nothing for it. The
 * design records that one build carries one backend and that the property is
 * structural with no run-time observable.
 */

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

	/* The run phase of one quantum asks `g2::runQuantum` for `want` MCU cycles
	 * and `Board::runMcu` supplies them. An installed runner is asked for the
	 * same want and must answer with the cycles it actually spent, so the
	 * cycle-debt block's arithmetic is untouched: a runner that returns less
	 * than it was asked for is the ordinary short-spend case that block already
	 * floors at zero, and no credit is banked for either party.
	 *
	 * The quantum order is stated once, in Scheduler::runFrames. The one thing a
	 * debugger needs that the quantum does not give it is a decision point
	 * between two MCU instructions, which is where a breakpoint compare has to
	 * happen; this is that point and it is nothing else. */
	class McuRunner
	{
	public:
		virtual ~McuRunner() = default;

		/* Answers the cycles actually spent, which may be fewer than `_want`. */
		virtual uint32_t runMcu(uint32_t _want) noexcept = 0;
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

			/* The position-to-port order the chain is wired by. Empty is the
			 * default and the production value.
			 *
			 * Empty means derive it from the machine. The firmware chooses which
			 * hardware port carries which chain position and it does not choose
			 * the identity; chainOrder.h carries the derivation and why it can
			 * only be answered once the firmware has run. A Scheduler built from
			 * an empty order therefore leaves its ports idle and wires the chain
			 * on the first quantum after the first program lands.
			 * chainAttached() is where a caller reads whether that has happened.
			 *
			 * A harness that drives the ESAIs itself has no firmware to ask and
			 * names the order it wants. A non-empty order is wired at
			 * construction and is not replaced by a later derivation.
			 *
			 * An order whose length is not `dspCount`, or that is not a
			 * permutation of the slots, answers Status::BadChainOrder in
			 * create() and yields no object. */
			std::vector<unsigned> chainOrder{};

			/* NULL MEANS NO RECORDING, and it is the default. */
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

		/* A null argument restores `Board::runMcu`. The Scheduler borrows the
		 * runner and never destroys it, so a runner must outlive the Scheduler
		 * or remove itself first -- GdbStub's destructor does the latter. */
		void setMcuRunner(McuRunner* _runner) noexcept { m_mcuRunner = _runner; }

		McuRunner* mcuRunner() const noexcept { return m_mcuRunner; }

		/* The boot-to-play transition, and the boot thread's last Scheduler
		 * action. It leaves the play phase's initial condition and nothing
		 * else: the CodecSource empty, the CodecSink holding exactly
		 * `lookaheadFrames` frames, every chain-health counter zero and
		 * the recorded owning-thread identity cleared. It touches no emulated
		 * machine state, clears no fault, and does not reset the frame index --
		 * its own `lookaheadFrames` quanta advance it by that much. */
		void beginPlayPhase() noexcept;

		/* Host input. Returns the number of frames the CodecSource accepted. A
		 * return below `_frames` means the queue refused the remainder, which
		 * the queue capacity makes unreachable in a correct build -- so a short
		 * return is a defect report and overflowFrames() counts it. */
		size_t push(const Frame* _in, size_t _frames) noexcept;

		/* Host output. Returns the frames actually taken. The part the sink
		 * could not supply reads as silence and raises underflowFrames by the
		 * shortfall. */
		size_t pull(Frame* _out, size_t _frames) noexcept;

		/* The virtual clock. It advances once for each quantum and nothing
		 * else moves it. */
		uint64_t frameIndex() const noexcept;

		/* The chain-health counters. None of them influences emulation and all
		 * are zeroed by beginPlayPhase.
		 *
		 * The first three read through a baseline this object keeps: they live
		 * on the ChainAdapter, which exposes no way to zero them, and zeroing
		 * the adapter is not an option either -- its mailboxes are emulated
		 * state and beginPlayPhase may not touch that. So the Scheduler's
		 * counter is the adapter's reading minus the reading taken at the last
		 * zeroing. */
		uint64_t underrunFrames(unsigned _position) const noexcept;
		uint64_t secondBusUnderrunFrames(unsigned _position) const noexcept;
		uint64_t phaseErrorFrames(unsigned _position) const noexcept;
		uint64_t starvedFrames() const noexcept;
		uint64_t overflowFrames() const noexcept;
		uint64_t droppedFrames() const noexcept;
		uint64_t underflowFrames() const noexcept;

		/* Both are emulated-cycle quantities and neither measures host time. A
		 * non-zero value is a finding, not necessarily a failure.
		 *
		 * Context index: 0 is the MCU and 1 .. dspCount are the DSPs,
		 * ascending. An index above dspCount reads back zero rather than
		 * running off the end. */
		int64_t  cycleDebt(unsigned _contextIndex) const noexcept;
		uint64_t longDispatchQuanta(unsigned _contextIndex) const noexcept;

		/* A fault is sticky: once a context has faulted, nothing but reset()
		 * clears it, and the faulted context is never dispatched again.
		 *
		 * `faulted()` is the disjunction over every context and is what the
		 * Device reads after runFrames returns. Context index: 0 is the MCU and
		 * 1 .. dspCount are the DSPs, ascending -- the same index cycleDebt and
		 * longDispatchQuanta take. An index above dspCount reads back false and
		 * JobFault::None rather than running off the end.
		 *
		 * Nothing throws and a release build removes an assertion, so a fault
		 * is observable through these three and through nothing else. */
		bool     faulted() const noexcept;
		bool     contextFaulted(unsigned _contextIndex) const noexcept;
		JobFault contextFault(unsigned _contextIndex) const noexcept;

		/* scheduler.cpp carries the block's layout.
		 *
		 * The snapshot is a flat byte block and it carries a version word.
		 * `stateLoad` reports through g2::Status rather than returning void:
		 * nothing throws, a release build removes an assertion, and a void
		 * return would leave the version word with nothing to refuse to --
		 * which is the silent acceptance the word exists to prevent.
		 *
		 * CallbackTimer is not part of it. It carries no emulated state, and a
		 * state file recorded on a fast machine must load identically on a slow
		 * one. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		/* The boot thread's call. It returns this object and the machine it
		 * drives to the state a freshly created Scheduler is in: every fault
		 * cleared and every context back in the dispatch set, the virtual clock
		 * at frame 0, the boot regime, both codec queues empty, every counter
		 * and every debt zero, the recorded owning thread cleared, and the
		 * emulated memories the objects below own zeroed.
		 *
		 * What it does not zero is stated in board.cpp at Board::reset. */
		void reset() noexcept;

		/* Whether the chain has been wired yet. With an empty Config::chainOrder
		 * the wiring is late -- the position-to-port order is read out of the
		 * booted machine, chainOrder.h carries why.
		 *
		 * False means the ports are still idle: every receive answers silence,
		 * every transmit is discarded and no audio reaches the ChainAdapter.
		 *
		 * A Scheduler built from a named order reports true from construction. */
		bool chainAttached() const noexcept;

		/* runFrames records the calling thread on its first call after each
		 * clearing, and beginPlayPhase clears the record so the first runFrames
		 * of the play phase re-establishes it on the audio thread. A
		 * default-constructed id means no owner is recorded. */
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

		/* Null means Board::runMcu, so a production Scheduler pays one null
		 * check for each quantum and nothing else. */
		McuRunner* m_mcuRunner = nullptr;

		/* HELD BY VALUE, design section 13.10.5: the Scheduler owns exactly one
		 * ChainAdapter. Its four constructor arguments come from the Config.
		 *
		 * The callbacks it hands to the Board's ESAIs outlive it, and this
		 * object installs no uninstaller. A destructor that cleared them would
		 * be new lifetime machinery in a file whose members' declaration order
		 * is already load-bearing, so the rule is the same one the Board obeys
		 * internally: a Scheduler is destroyed before the Board it was given,
		 * and nothing drives an ESAI register between one Scheduler's death and
		 * the next one's install. */
		ChainAdapter m_chain;

		/* The MCU context, context index 0. It carries the rate, the rational
		 * accumulator, the cycle debt and the long-dispatch counter, and
		 * g2::runQuantum wraps Board::runMcu with it. It is not a JobContext
		 * and it does not enter the job array, which stays at exactly
		 * kJobCount. */
		McuContext m_mcu;

		/* The authoritative virtual frame index. Every phase of one quantum
		 * reads the SAME value, and it advances once for each quantum. */
		uint64_t m_frameIndex = 0;

		/* The caller does not select the regime: a Scheduler is born in Boot and
		 * beginPlayPhase is the one thing that moves it to Play. In Boot a
		 * quantum runs the swap and the run phase only -- no ingress, no
		 * egress, neither queue touched. In Play all four phases run. Without
		 * the boot regime the sink fills after L + B boot quanta and the
		 * scheduler stops part-way through the boot. */
		enum class CodecRegime { Boot, Play };

		CodecRegime m_regime = CodecRegime::Boot;

		/* Both capacities are lookaheadFrames + maxHostBlockFrames, and both
		 * rings are allocated once, here, and never resized by a quantum. */
		CodecSource m_source;
		CodecSink   m_sink;

		/* L, kept because beginPlayPhase needs it twice -- once to prime and
		 * once to count its own quanta -- and the Config is not stored. */
		unsigned m_lookaheadFrames;
		size_t   m_codecCapacity;

		/* The baseline of the adapter-owned counters, one for each position.
		 * The ChainAdapter cannot be zeroed without destroying emulated state,
		 * so the Scheduler subtracts the reading it took at the last
		 * zeroing. */
		std::vector<uint64_t> m_underrunBase;
		std::vector<uint64_t> m_secondUnderrunBase;
		std::vector<uint64_t> m_phaseErrorBase;

		/* A default-constructed id means no owner is recorded, which is the
		 * state beginPlayPhase leaves and the state a fresh Scheduler starts
		 * in. */
		std::thread::id m_owner{};

		/* The sticky fault latch, one entry for each context index: entry 0 is
		 * the MCU and 1 .. kJobCount are the DSPs. It is the Scheduler's record
		 * and not the job's: a job's own `base.fault` is overwritten by the
		 * next quantum that dispatches it, and the MCU has no fault field at
		 * all -- Board::faulted() is one bit with no code beside it. Latching
		 * here is what makes contextFault answer for every index rather than
		 * for the DSPs only. */
		JobFault m_fault[1u + kJobCount]{};

		/* The disjunction over the latch, kept rather than computed. It is what
		 * the Device reads after every runFrames. */
		bool m_faulted = false;

		/* The job array, built once. `Job::ctx` points at each context's
		 * LEADING JobContext, which dspContext.h's two static_asserts are what
		 * make legal. */
		DspContext    m_contexts[kJobCount]{};
		Executor::Job m_jobs[kJobCount]{};

		/* The dispatch set, a second array rather than a count into the first.
		 * Executor::run takes a contiguous array and a count, so a faulted
		 * context in the middle cannot be skipped by lowering the count; the
		 * live jobs are compacted into this array instead, and m_jobs keeps its
		 * position-indexed order so that nothing else has to learn about the
		 * compaction.
		 *
		 * A faulted context is never dispatched again, and reset() is the only
		 * thing that puts one back. */
		Executor::Job m_liveJobs[kJobCount]{};
		size_t        m_liveCount = 0;

		/* Rebuilds m_liveJobs from m_fault. Called at construction, whenever a
		 * fault latches, and by reset(). */
		void rebuildDispatchSet() noexcept;

		/* The late chain attach. The position-to-port order the chain is wired
		 * by is read out of the booted machine -- chainOrder.h carries the
		 * derivation -- and this object is constructed before the firmware has
		 * run. A caller that named an order in the Config is wired at
		 * construction and this never fires; every other caller is wired by
		 * this, which tries once for each quantum until the machine can answer
		 * and then never again.
		 *
		 * m_chainOrder is scratch, sized at construction so that the attempt
		 * allocates nothing inside a quantum. It carries no meaning between
		 * calls; readChainOrder overwrites every entry.
		 *
		 * m_chainAttached is not part of the state snapshot. It records what
		 * this object has wired and not what the emulated machine holds. */
		void attachChainIfOrderKnown() noexcept;

		std::vector<unsigned> m_chainOrder;
		bool                  m_chainAttached = false;

		/* The order the caller named, kept because reset() has to tell the two
		 * cases apart. Empty means the order was derived from the machine,
		 * which a reset invalidates; non-empty means the caller supplied it,
		 * which a reset says nothing about. */
		std::vector<unsigned> m_configuredChainOrder;

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
