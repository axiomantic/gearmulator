/* The backend rule: `Scheduler::create` succeeds only when
 * `config.backend == Backend::Jit` and `dsp56k::g_useJIT` is true. Any other
 * combination returns a null `Scheduler` object.
 *
 * What this test does:
 *
 *   1. With backend == Backend::Jit and `g_useJIT` true,
 *      `Scheduler::create` returns a non-null object.
 *   2. With backend == Backend::Interpreter, `Scheduler::create` returns
 *      null, regardless of `g_useJIT`.
 *
 * The second case is unconditional and runs on every build. The first is
 * conditional on the build: in an interpreter build no Scheduler can be created
 * at all, because `runDspCycles` cannot terminate there -- the DSP's `m_cycles`
 * counter is never written. The test makes the first case conditional so that
 * the assertion does not pin a property the build cannot exercise.
 *
 * It does not assert "one backend for one run". `g_useJIT` is
 * `static constexpr` at dsp.h:36 and the dispatch at dsp.h:172-178 is a plain
 * `if`, not `if constexpr`, so the branch folds at compile time because its
 * condition is a constant expression. A test asserting structural
 * one-backend-per-build would pass by exercising nothing.
 *
 * The build mode is printed in the first line, so the configuration is visible
 * in the test log without a separate device.
 */

#include "board.h"
#include "executor.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"

#include <cstdio>
#include <memory>

namespace
{
	int g_failures = 0;

	void check(const bool _condition, const char* const _what)
	{
		if(_condition)
		{
			std::printf("ok   %s\n", _what);
			return;
		}
		std::printf("FAIL %s\n", _what);
		++g_failures;
	}
}

int main()
{
	std::printf("t0_backend_rule: g_useJIT = %s\n",
		dsp56k::g_useJIT ? "true" : "false");

	/* The factory's remaining arguments. The signature widened to
	 * `(const Config&, Executor&, Board&, Status&)`, and this check still
	 * reads only the return value: the status is written and deliberately
	 * not asserted, for the reason the header comment above gives. */
	g2::SerialExecutor executor;
	g2::Board          board;
	g2::Status         status{};

	/* ---------------- case 1: backend == Backend::Jit
	 *
	 * The conditional result. The design says the rule is structural, so
	 * the assertion is conditional on the same constant the rule reads.
	 *
	 * A release build removes the branch the rule does not take, so the
	 * assertion is the only observable of the rule's reach: the test
	 * names the build mode above and the case below so the outcome is
	 * traceable to a configuration. */
	{
		g2::Scheduler::Config cfg;
		cfg.backend = g2::Backend::Jit;

		std::unique_ptr<g2::Scheduler> s = g2::Scheduler::create(cfg, executor, board, status);

		if(dsp56k::g_useJIT)
		{
			check(s != nullptr,
				"Backend::Jit with g_useJIT=true returns non-null");
		}
		else
		{
			check(s == nullptr,
				"Backend::Jit with g_useJIT=false returns null");
		}
	}

	/* ---------------- case 2: backend == Backend::Interpreter
	 *
	 * Unconditional. The semantic cross-check harness drives DSP::exec
	 * directly and never constructs a Scheduler, so the enumerator exists
	 * but is never accepted by create(). The rule rejects it on every
	 * build. */
	{
		g2::Scheduler::Config cfg;
		cfg.backend = g2::Backend::Interpreter;

		std::unique_ptr<g2::Scheduler> s = g2::Scheduler::create(cfg, executor, board, status);

		check(s == nullptr, "Backend::Interpreter returns null");
	}

	if(g_failures != 0)
	{
		std::printf("t0_backend_rule: %d check(s) failed\n", g_failures);
		return 1;
	}

	std::printf("t0_backend_rule: all checks passed\n");
	return 0;
}
