/* scheduler.h -- the G2 scheduler's header. Tasks SCH-17, SCH-18 and SCH-19.
 *
 * Design sections 11.4.1, 11.4.3, 13.5, 13.10.5, 13.10 rule 4, 13.4.5, 13.6 and
 * 13.6.2. Plan section 14.5.
 *
 * WHAT IS DECLARED HERE.
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
 *    because the swap, the ingress, the panel, the start-of-frame tick, the
 *    MCU and the egress all run SERIALLY in the Scheduler, outside the
 *    Executor -- so the order they run in has no other decider. The
 *    alternative was four accessors, one for each thing a check must reach,
 *    and four pieces of surface that only move the wall are a worse object
 *    than one seam that makes a real mutation go red.
 *
 * 5. SCH-19's `runFrames`, the quantum entry point, and the private
 *    constructor that wires the Executor and the Board in.
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

#include "chainAdapter.h"
#include "executor.h"
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

	/* THE PHASE TAGS OF ONE QUANTUM, design section 13.5's order. */
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
		/* THE CONFIG STRUCT. Design section 13.10.5's fields, plus the test
		 * override below.
		 *
		 * EVERY DEFAULT IS A LEGAL VALUE, so a caller that perturbs one field
		 * is rejected for that field and for nothing else. Where a shipped
		 * constant exists the default IS that constant; `lookaheadFrames` and
		 * `maxHostBlockFrames` have none -- measurement register row 11 leaves
		 * `L` to spike criterion (c), and `B` is computed at run time by the
		 * plugin wrapper from the host's maximum block -- so those two carry
		 * the smallest legal value instead. THAT IS ALL THE DEFAULTS CLAIM:
		 * they are not the shipped machine's configuration and no caller may
		 * read them as one. */
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

			/* THE ESCAPE HATCH FROM THE TWO EQUALITY ROWS, and nothing else.
			 * Measurement register rows 9 and 10 require tests at H = 1 and
			 * H = 2 and at divider 1 and 4, which a strict equality against
			 * the build constants would forbid.
			 *
			 * IT DOES NOT REACH THE ZERO ROWS. A hop of zero collapses the hop
			 * and a divider of zero is a zero divisor, and neither becomes
			 * legal because a test asked for it.
			 *
			 * FALSE BY DEFAULT, so the escape is taken only by a caller that
			 * names it. PLG-17's golden-render path asserts it is false, so
			 * the one path that records a reference cannot take it. */
			bool          testOverride          = false;

			/* NULL MEANS NO RECORDING, and it is the default. */
			TraceSink*    trace                 = nullptr;
		};

		/* THE FACTORY, AND THE SINGLE REJECTION POINT. Defined in
		 * scheduler.cpp.
		 *
		 * `_outStatus` is written on every path, including the successful one.
		 * A null return with `Status::Unset` left in place would be a path that
		 * rejected without saying why.
		 *
		 * NO EXCEPTION, NO ASSERTION. Design section 13.10 rule 2 forbids
		 * throwing, and a release build removes an assertion, so the rejections
		 * are observable in a release build through the return value and the
		 * status and through nothing else. */
		static std::unique_ptr<Scheduler> create(const Config& _config, Executor& _executor, Board& _board,
			Status& _outStatus);

		/* THE QUANTUM ENTRY POINT, AND THE ONLY WAY A QUANTUM HAPPENS. It turns
		 * `_frames` whole quanta, each in the order of design section 13.5.
		 *
		 * `void` AND `noexcept`, because design section 13.10 rule 2 forbids
		 * throwing and SCH-23 has the Device take a fault AFTER this returns,
		 * through the Scheduler's own fault surface. A return value carrying
		 * the fault would give that property a second home.
		 *
		 * A FRAME COUNT AND NOT A QUANTUM COUNT. `framesPerQuantum` is fixed at
		 * 1 and every calling site in the design counts frames.
		 *
		 * THE OWNING THREAD IS THE CALLER'S AND THIS OBJECT CREATES NONE. The
		 * serial executor runs the jobs on this same thread; a parallel one may
		 * use workers inside run(), which returns only when every job has. */
		void runFrames(size_t _frames) noexcept;

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
		/* Private. SCH-19 wires the Executor and the Board in, so the
		 * constructor that takes them is its concern; `create` is the only
		 * caller and it runs the whole rejection table first, so every value
		 * read below has already been vetted. */
		Scheduler(const Config& _config, Executor& _executor, Board& _board);

		/* One trace record, or nothing at all when no sink was supplied. */
		void mark(TracePhase _phase, uint64_t _frameIndex) const noexcept;

		/* BOTH ARE BORROWED AND NEITHER IS OWNED, and both must OUTLIVE this
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

		/* NULL MEANS NO RECORDING, so a production Scheduler pays one null
		 * check for each phase of each quantum and nothing else. */
		TraceSink* m_trace;

		/* HELD BY VALUE, design section 13.10.5: the Scheduler owns exactly one
		 * ChainAdapter. Its four constructor arguments come from the Config.
		 *
		 * THE CALLBACKS IT HANDS TO THE BOARD'S ESAIs OUTLIVE IT, and this
		 * object installs no uninstaller. A destructor that cleared them would
		 * be new lifetime machinery in a file whose members' declaration order
		 * is already load-bearing, so the rule is the same one the Board obeys
		 * internally: a Scheduler is destroyed before the Board it was given,
		 * and nothing drives an ESAI register between one Scheduler's death and
		 * the next one's install. */
		ChainAdapter m_chain;

		/* THE MCU's CYCLES-FOR-EACH-FRAME RATIONAL AND ITS ACCUMULATOR. The
		 * budget/want/debt block for the MCU context is SCH-30's; this is the
		 * allocation alone, which is what `runMcu` takes. */
		Rational m_mcuRate;
		uint32_t m_mcuAcc     = 0;

		/* THE AUTHORITATIVE VIRTUAL FRAME INDEX. Every phase of one quantum
		 * reads the SAME value, and it advances once for each quantum. */
		uint64_t m_frameIndex = 0;

		/* NO CODEC-QUEUE MEMBER AND NO CODEC `Frame` MEMBER STANDS HERE, and
		 * that is deliberate rather than unfinished. The ingress and the egress
		 * are PLAY REGIME ONLY; this class carries no regime member, so it is
		 * the boot machine by construction and a quantum of it runs the swap
		 * and the run phase alone. SCH-22 adds the regime, the two adapter
		 * calls and the queue members that feed them, in one place.
		 *
		 * THE JOB ARRAY, BUILT ONCE. `Job::ctx` points at each context's
		 * LEADING JobContext, which dspContext.h's two static_asserts are what
		 * make legal. */
		DspContext    m_contexts[kJobCount]{};
		Executor::Job m_jobs[kJobCount]{};
	};

	/* NEITHER COPYABLE NOR MOVABLE, as a compile-time property so that it
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
