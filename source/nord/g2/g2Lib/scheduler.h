/* scheduler.h -- the G2 scheduler's header. Tasks SCH-17 and SCH-18.
 *
 * Design sections 11.4.1, 11.4.3, 13.10.5, 13.10 rule 4, 13.4.5, 13.6 and
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

#include <memory>

#include "chainAdapter.h"
#include "status.h"

#include "g2/timebase.h"

#include "dsp56kEmu/dsp.h"

namespace g2
{
	class Board;
	class Executor;

	/* The backend. Fixed for the whole BINARY, by dsp56300's own
	 * `static constexpr bool g_useJIT` at dsp.h:36. This enum therefore
	 * records which backend the binary was built with; it does not select
	 * one. §11.4.1.
	 *
	 * ONE RULE GOVERNS `Config::backend`, §11.4.3: create() succeeds only
	 * when backend == Backend::Jit AND `g_useJIT` is true. Any other
	 * combination returns a null Scheduler object. */
	enum class Backend { Jit, Interpreter };

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

	private:
		/* Private. SCH-19 declares the Executor and Board wiring and the
		 * owning-thread model, and the constructor that takes them is its
		 * concern. SCH-17 constructs nothing but the fact of success. */
		Scheduler() = default;
	};
}
