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

		/* The MCU's cycles-for-each-frame rational and its accumulator: the
		 * allocation alone, which is what `runMcu` takes. */
		Rational m_mcuRate;
		uint32_t m_mcuAcc     = 0;

		/* The authoritative virtual frame index. Every phase of one quantum
		 * reads the SAME value, and it advances once for each quantum. */
		uint64_t m_frameIndex = 0;

		/* No codec-queue member and no codec `Frame` member stands here, and
		 * that is deliberate rather than unfinished. The ingress and the egress
		 * are play regime only; this class carries no regime member, so it is
		 * the boot machine by construction and a quantum of it runs the swap
		 * and the run phase alone.
		 *
		 * `Job::ctx` points at each context's
		 * LEADING JobContext, which dspContext.h's two static_asserts are what
		 * make legal. */
		DspContext    m_contexts[kJobCount]{};
		Executor::Job m_jobs[kJobCount]{};
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
