/* The G2 scheduler's header.
 *
 * The rule this factory implements: `Scheduler::create` succeeds only when
 * `config.backend == Backend::Jit` and `dsp56k::g_useJIT` is true. Any other
 * combination returns a null `Scheduler` object.
 *
 * In a JIT build only `Backend::Jit` is accepted. In an interpreter build no
 * `Scheduler` can be created at all, which is the correct outcome because
 * `runDspCycles` cannot terminate in such a build: the DSP's `m_cycles` counter
 * is never written. The `Interpreter` enumerator stays because the semantic
 * cross-check harness drives `DSP::exec` directly and never constructs a
 * `Scheduler`.
 *
 * `g_useJIT` is a `static constexpr` at dsp.h:36, so the compiler folds the
 * second branch at compile time and the interpreter-build path is a literal
 * `return nullptr` with no run-time cost.
 */

#pragma once

#include <memory>

#include "dsp56kEmu/dsp.h"

namespace g2
{
	/* The backend. Fixed for the whole binary, by dsp56300's own
	 * `static constexpr bool g_useJIT` at dsp.h:36. This enum records which
	 * backend the binary was built with; it does not select one. */
	enum class Backend { Jit, Interpreter };

	class Scheduler
	{
	public:
		struct Config
		{
			Backend backend = Backend::Jit;
		};

		/* No exception and no assertion: the rule survives a release build and
		 * is observable through the null-vs-non-null distinction. */
		static std::unique_ptr<Scheduler> create(const Config& _config)
		{
			if(_config.backend != Backend::Jit)
				return nullptr;
			if(!dsp56k::g_useJIT)
				return nullptr;
			return std::unique_ptr<Scheduler>(new Scheduler);
		}

	private:
		Scheduler() = default;
	};
}
