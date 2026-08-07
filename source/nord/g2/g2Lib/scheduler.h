/* scheduler.h -- the G2 scheduler's header. Task SCH-17.
 *
 * Design sections 11.4.1, 11.4.3 and 13.10.5. Plan section 14.5.
 *
 * SCH-17 OWNS THREE DECLARATIONS HERE.
 *
 * 1. The `Backend` enum. SCH-18 and later owners of this header extend the
 *    Config struct and the create() factory; SCH-17 establishes the enum and
 *    the single factory rule they all share.
 *
 * 2. The `Scheduler::Config` struct, MINIMUM for the backend rule. SCH-19
 *    adds the fields design section 13.10.5 names -- dspCount,
 *    framesPerQuantum, lookaheadFrames, maxHostBlockFrames, hopFrames,
 *    secondBusTopology, secondBusFrameDivider, dspRate, mcuRate -- and the
 *    factory grows the same way. SCH-17 leaves them out on purpose: a
 *    header that names the ungoverned fields before SCH-19 creates them is
 *    a header that other sched tasks cannot extend.
 *
 * 3. The `Scheduler::create` factory, MINIMUM signature. SCH-19 widens it to
 *    `(const Config&, Executor&, Board&, Status& outStatus)` per design
 *    section 13.10.5. SCH-17 declares the subset the backend rule needs --
 *    just the Config -- so that the test of this task can call it.
 *
 * THE RULE THIS FACTORY IMPLEMENTS, STATED IN ONE SENTENCE.
 *
 *   `Scheduler::create` succeeds only when `config.backend == Backend::Jit`
 *    AND `dsp56k::g_useJIT` is true. Any other combination returns a null
 *    `Scheduler` object.
 *
 * Design section 11.4.3 states the rule and gives the three consequences: in
 * a JIT build only `Backend::Jit` is accepted; in an interpreter build no
 * `Scheduler` can be created at all, which is the correct outcome because
 * `runDspCycles` cannot terminate in such a build (the DSP's `m_cycles`
 * counter is never written); and the `Interpreter` enumerator stays because
 * the semantic cross-check harness of design section 11.4.3 drives
 * `DSP::exec` directly and never constructs a `Scheduler`.
 *
 * THE TEST DOES NOT LOOK AT `Status::BadBackend`. SCH-18 owns Status, and
 * a check that asserted the Status value here would create the cycle SCH-17
 * is the first writer of. The null-vs-non-null distinction is what proves
 * the rule, and it is observable without a Status type.
 *
 * WHY THE IMPLEMENTATION IS INLINE. The rule is one branch and one branch
 * only, and a header that grows through SCH-18, SCH-19, SCH-21, SCH-22,
 * SCH-23, SCH-24, SCH-28 and SCH-30 is a header that has no business
 * owning a translation unit of its own during this task. SCH-19 will open
 * `scheduler.cpp` and move everything that survives.
 *
 * `g_useJIT` IS A `static constexpr` AT dsp.h:36. The compiler folds the
 * second branch at compile time when the build is the kind that does not
 * carry the JIT, so the interpreter-build path is a literal `return nullptr`
 * with no run-time cost. The design records that one build carries one
 * backend and the property is structural with no run-time observable.
 */

#pragma once

#include <memory>

#include "dsp56kEmu/dsp.h"

namespace g2
{
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
		/* THE CONFIG STRUCT, MINIMUM for SCH-17's rule. SCH-19 adds the rest
		 * of design section 13.10.5's fields. SCH-17 declares one field on
		 * purpose: the rule this task owns depends on nothing else, and a
		 * wider declaration here is a header SCH-19 cannot extend without
		 * rewriting. */
		struct Config
		{
			Backend backend = Backend::Jit;
		};

		/* THE FACTORY, MINIMUM signature for SCH-17's rule. SCH-19 widens it
		 * to `(const Config&, Executor&, Board&, Status& outStatus)` per
		 * design section 13.10.5. The implementation is INLINE because the
		 * rule is one branch in one function and SCH-17 owns no translation
		 * unit of its own.
		 *
		 * NO EXCEPTION, NO ASSERTION. The rule survives a release build and
		 * is testable through the null-vs-non-null distinction the test
		 * makes. Design section 13.10 rule 2 forbids exceptions. */
		static std::unique_ptr<Scheduler> create(const Config& _config)
		{
			if(_config.backend != Backend::Jit)
				return nullptr;
			if(!dsp56k::g_useJIT)
				return nullptr;
			return std::unique_ptr<Scheduler>(new Scheduler);
		}

	private:
		/* Private. SCH-19 declares the Executor and Board wiring and the
		 * owning-thread model, and the constructor that takes them is its
		 * concern. SCH-17 constructs nothing but the fact of success. */
		Scheduler() = default;
	};
}
