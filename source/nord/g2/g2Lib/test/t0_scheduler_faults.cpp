/* The check of the Scheduler's fault surface.
 *
 * How a fault is forced, and why the seam is the Executor: dspJob writes no
 * fault today, and the three DSP values name conditions of the dsp56300
 * backend that a T0 fixture cannot produce on demand. The Executor is injected
 * into the factory, so a check-owned Executor that dispatches the real job and
 * then writes the fault field of one context reproduces exactly the state the
 * Scheduler must read: the fault lives in the job's own context and the
 * Scheduler reads it after run() returns. Every other object below is the real
 * one: a real Board, its real DSP set, the real ChainAdapter and both real
 * codec queues.
 *
 * The MCU fault is not forced through a seam at all. A default-constructed
 * Board carries a MemoryMapConfig whose every window has size 0, and an absent
 * window answers at no address at all, including address zero, so the first
 * instruction fetch of the first runMcu is a bus error and the core faults.
 * The rate 0/1 used by groups A and B keeps that from happening there: the
 * budget is zero at every quantum, so the role filler is never invoked.
 *
 * underrunFrames is deliberately not asserted. In this fixture it rises for
 * every position whether it faulted or not: no firmware is downloaded, every
 * slot's run gate is shut, no transmit callback ever fires, and advanceAll
 * counts an audio-bus underrun at every position for every quantum by
 * construction. An assertion on it would pass identically against a Scheduler
 * that kept dispatching the faulted context. The dispatch-count assertion is
 * the one that separates those two implementations.
 *
 * No case here is a language assert() and no case catches an exception, so
 * this file reports identically under NDEBUG and without it.
 */

#include "board.h"
#include "dspContext.h"
#include "executor.h"
#include "frame.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

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
			static_cast<unsigned long long>(_observed), static_cast<unsigned long long>(_expected));
		++g_failures;
	}

	constexpr unsigned kDspCount     = static_cast<unsigned>(g2::kJobCount);
	constexpr unsigned kLookahead    = 4;
	constexpr unsigned kMaxHostBlock = 3;
	constexpr size_t   kCapacity     = static_cast<size_t>(kLookahead) + kMaxHostBlock;

	const char* faultName(const g2::JobFault _f)
	{
		switch(_f)
		{
		case g2::JobFault::None:               return "None";
		case g2::JobFault::IllegalInstruction: return "IllegalInstruction";
		case g2::JobFault::MemoryFault:        return "MemoryFault";
		case g2::JobFault::BackendFault:       return "BackendFault";
		case g2::JobFault::CoreHalted:         return "CoreHalted";
		}
		return "?";
	}

	/* The Executor this check owns. It runs every job the Scheduler hands it,
	 * in order, on the calling thread -- which is what SerialExecutor does --
	 * and it does two things beyond that:
	 *
	 *   It counts the dispatches for each chain position. That count is the
	 *   only observable of "never dispatched again": the Scheduler declares no
	 *   accessor for its dispatch set and this file adds none to it.
	 *
	 *   When armed, it writes one JobFault into one context after that
	 *   context's job has returned, which is where a real fault would land.
	 *
	 * The position is read back through the JobContext head, which
	 * dspContext.h's two static_asserts are what make legal. */
	class CountingFaultExecutor final : public g2::Executor
	{
	public:
		void run(const Job* const _jobs, const size_t _count) noexcept override
		{
			m_lastCount = _count;

			for(size_t i = 0; i < _count; ++i)
			{
				auto* const ctx = reinterpret_cast<g2::DspContext*>(_jobs[i].ctx);

				if(ctx->position < kDspCount)
					++m_dispatches[ctx->position];

				_jobs[i].fn(_jobs[i].ctx);

				if(m_armed && ctx->position == m_target)
				{
					_jobs[i].ctx->fault = m_fault;
					m_armed             = false;
				}
			}
		}

		bool isSerial() const noexcept override { return true; }

		void arm(const unsigned _position, const g2::JobFault _fault) noexcept
		{
			m_target = _position;
			m_fault  = _fault;
			m_armed  = true;
		}

		uint64_t dispatches(const unsigned _position) const noexcept
		{
			return _position < kDspCount ? m_dispatches[_position] : 0;
		}

		size_t lastCount() const noexcept { return m_lastCount; }

	private:
		uint64_t     m_dispatches[kDspCount]{};
		size_t       m_lastCount = 0;
		unsigned     m_target    = 0;
		g2::JobFault m_fault     = g2::JobFault::None;
		bool         m_armed     = false;
	};

	/* Asserts the whole fault surface at every context index 0 .. dspCount.
	 * `_faultedIndex` is the one index expected to carry `_expected`; passing
	 * an index above dspCount asserts that no index carries a fault at all.
	 *
	 * Walking every index is the point. A latch that set the whole array, or
	 * that was off by one between the MCU's index 0 and the DSPs' 1 .. dspCount,
	 * passes an assertion that reads only the index it expects to be set. */
	void checkFaultSurface(const g2::Scheduler& _s, const unsigned _faultedIndex,
		const g2::JobFault _expected, const char* const _when)
	{
		for(unsigned i = 0; i <= kDspCount; ++i)
		{
			const bool         wantFaulted = (i == _faultedIndex);
			const g2::JobFault wantFault   = wantFaulted ? _expected : g2::JobFault::None;

			char what[256];

			std::snprintf(what, sizeof(what), "%s: contextFaulted(%u) is %s", _when, i,
				wantFaulted ? "true" : "false");
			check(_s.contextFaulted(i) == wantFaulted, what);

			std::snprintf(what, sizeof(what), "%s: contextFault(%u) is %s (observed %s)", _when, i,
				faultName(wantFault), faultName(_s.contextFault(i)));
			check(_s.contextFault(i) == wantFault, what);
		}
	}

	/* One whole run for one JobFault value. Every object is built fresh so
	 * that no case inherits another's latch. */
	void runFaultCase(const g2::JobFault _fault, const unsigned _position)
	{
		char what[256];

		/* The Board is declared first: every context borrows a core, two ESAI
		 * ports and a landed flag owned by a slot of the Board's DSP set. */
		g2::Board               board;
		CountingFaultExecutor   executor;

		g2::Scheduler::Config config;
		config.lookaheadFrames    = kLookahead;
		config.maxHostBlockFrames = kMaxHostBlock;
		config.mcuRate            = { 0, 1 };

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		if(!scheduler)
		{
			std::snprintf(what, sizeof(what), "%s: the Config yields a Scheduler", faultName(_fault));
			check(false, what);
			return;
		}

		g2::Scheduler& s = *scheduler;

		/* The known positive, and it runs before any "is zero" read. Boot
		 * quanta with nothing armed must dispatch every position once each.
		 * Without this, "the faulted position stopped" is a comparison of a
		 * count that was never able to move. */
		constexpr uint64_t kArmingQuanta = 6;

		s.runFrames(kArmingQuanta);

		for(unsigned p = 0; p < kDspCount; ++p)
		{
			std::snprintf(what, sizeof(what),
				"%s: KNOWN POSITIVE: position %u was dispatched once for each of the %llu boot quanta",
				faultName(_fault), p, static_cast<unsigned long long>(kArmingQuanta));
			checkEqual(executor.dispatches(p), kArmingQuanta, what);
		}

		std::snprintf(what, sizeof(what),
			"%s: KNOWN POSITIVE: the whole job array is dispatched while no context has faulted",
			faultName(_fault));
		checkEqual(executor.lastCount(), g2::kJobCount, what);

		std::snprintf(what, sizeof(what), "%s: faulted() is false before the fault is forced",
			faultName(_fault));
		check(!s.faulted(), what);

		std::snprintf(what, sizeof(what), "%s before the fault", faultName(_fault));
		checkFaultSurface(s, kDspCount + 1u, g2::JobFault::None, what);

		executor.arm(_position, _fault);
		s.runFrames(1);

		std::snprintf(what, sizeof(what), "%s: faulted() is true once one context has faulted",
			faultName(_fault));
		check(s.faulted(), what);

		std::snprintf(what, sizeof(what), "%s after the fault", faultName(_fault));
		checkFaultSurface(s, _position + 1u, _fault, what);

		/* The faulting quantum itself did dispatch the faulted position -- the
		 * fault is written when its job returns -- so its count stands at
		 * kArmingQuanta + 1 and every other position's rises by the later
		 * quanta as well. Both expected values are derived from the two quanta
		 * counts and neither is written as a literal. */
		constexpr uint64_t kLaterQuanta = 5;

		s.runFrames(kLaterQuanta);

		for(unsigned p = 0; p < kDspCount; ++p)
		{
			const uint64_t expected = (p == _position)
				? kArmingQuanta + 1
				: kArmingQuanta + 1 + kLaterQuanta;

			std::snprintf(what, sizeof(what),
				"%s: position %u dispatch count after the fault (the faulted context stops, "
				"every other one keeps running)", faultName(_fault), p);
			checkEqual(executor.dispatches(p), expected, what);
		}

		std::snprintf(what, sizeof(what),
			"%s: the job array handed to the Executor is one shorter once a context has faulted",
			faultName(_fault));
		checkEqual(executor.lastCount(), g2::kJobCount - 1u, what);

		std::snprintf(what, sizeof(what), "%s: faulted() is STICKY across the later quanta",
			faultName(_fault));
		check(s.faulted(), what);

		std::snprintf(what, sizeof(what), "%s, still sticky", faultName(_fault));
		checkFaultSurface(s, _position + 1u, _fault, what);

		/* The dispatch set is proven restored rather than merely reported
		 * clear. A reset that cleared the surface and left the faulted context
		 * out of the job array would pass the surface assertions and fail the
		 * count below. */
		s.reset();

		std::snprintf(what, sizeof(what), "%s: reset() clears faulted()", faultName(_fault));
		check(!s.faulted(), what);

		std::snprintf(what, sizeof(what), "%s after reset", faultName(_fault));
		checkFaultSurface(s, kDspCount + 1u, g2::JobFault::None, what);

		std::snprintf(what, sizeof(what), "%s: reset() returns the virtual clock to frame 0",
			faultName(_fault));
		checkEqual(s.frameIndex(), 0u, what);

		std::snprintf(what, sizeof(what), "%s: reset() clears the recorded owning thread",
			faultName(_fault));
		check(s.owningThread() == std::thread::id{}, what);

		constexpr uint64_t kAfterReset = 2;

		const uint64_t before = executor.dispatches(_position);

		s.runFrames(kAfterReset);

		std::snprintf(what, sizeof(what),
			"%s: the once-faulted position is dispatched again after reset()", faultName(_fault));
		checkEqual(executor.dispatches(_position), before + kAfterReset, what);

		std::snprintf(what, sizeof(what),
			"%s: the whole job array is dispatched again after reset()", faultName(_fault));
		checkEqual(executor.lastCount(), g2::kJobCount, what);

		/* reset() left the object in the boot regime, so beginPlayPhase runs
		 * its own L priming quanta and leaves exactly L frames in the sink.
		 * Further play quanta fill it to its capacity L + B, which is derived
		 * from the two Config fields and never written as a literal. The
		 * request is that whole capacity. */
		scheduler->beginPlayPhase();

		executor.arm(_position, _fault);

		const size_t playQuanta = kCapacity - kLookahead;

		s.runFrames(playQuanta);

		std::snprintf(what, sizeof(what), "%s: the fault forced in the play regime is latched",
			faultName(_fault));
		check(s.faulted(), what);

		{
			std::vector<g2::Frame> out(kCapacity);

			const size_t pulled = s.pull(out.data(), kCapacity);

			std::snprintf(what, sizeof(what),
				"%s: pull returns the FULL request while a context is faulted", faultName(_fault));
			checkEqual(pulled, kCapacity, what);

			std::snprintf(what, sizeof(what),
				"%s: the full request was satisfied, so no shortfall was counted", faultName(_fault));
			checkEqual(s.underflowFrames(), 0u, what);
		}
	}
}

int main()
{
	std::printf("t0_scheduler_faults: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	if(!dsp56k::g_useJIT)
	{
		/* In an interpreter build no Scheduler can be created at all, so the
		 * refusal is the only claim this file may make. */
		g2::Board             board;
		CountingFaultExecutor executor;

		g2::Scheduler::Config config;
		config.backend = g2::Backend::Jit;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		check(scheduler == nullptr, "an interpreter build yields no Scheduler");
		checkEqual(static_cast<uint64_t>(status), static_cast<uint64_t>(g2::Status::BadBackend),
			"an interpreter build reports BadBackend");

		std::printf("t0_scheduler_faults: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return g_failures == 0 ? 0 : 1;
	}

	/* Each DSP JobFault value is forced at a different chain position, so that
	 * a latch which happened to work at one index is not the whole evidence. */
	runFaultCase(g2::JobFault::IllegalInstruction, 0);
	runFaultCase(g2::JobFault::MemoryFault,        3);
	runFaultCase(g2::JobFault::BackendFault,       kDspCount - 1);

	/* Board::faulted() drives context index 0. The rate is 1/1 rather than
	 * 0/1, so the budget is one cycle at every quantum and the role filler is
	 * invoked -- which is what the other groups deliberately avoid. The
	 * Board's every memory window has size 0, so the first instruction fetch
	 * is a bus error. */
	{
		g2::Board             board;
		CountingFaultExecutor executor;

		g2::Scheduler::Config config;
		config.lookaheadFrames    = kLookahead;
		config.maxHostBlockFrames = kMaxHostBlock;
		config.mcuRate            = { 1, 1 };

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		if(!scheduler)
		{
			check(false, "the MCU-fault Config yields a Scheduler");
			std::printf("t0_scheduler_faults: %d failure(s) in %d case(s)\n", g_failures, g_cases);
			return 1;
		}

		g2::Scheduler& s = *scheduler;

		/* The bit must be observed false on this very Board before it is
		 * observed true, or "it is true afterwards" says nothing about whether
		 * this quantum set it. */
		check(!board.faulted(), "KNOWN NEGATIVE: the Board has not faulted before the first quantum");
		check(!s.faulted(),     "the Scheduler reports no fault before the first quantum");
		checkFaultSurface(s, kDspCount + 1u, g2::JobFault::None, "MCU, before the first quantum");

		s.runFrames(1);

		check(board.faulted(),
			"KNOWN POSITIVE: the first instruction fetch of an unmapped Board is a bus error");
		check(s.faulted(), "the Scheduler reports the MCU fault");

		/* Index 0 carries CoreHalted and no DSP index carries anything. */
		checkFaultSurface(s, 0, g2::JobFault::CoreHalted, "MCU, after the faulting quantum");

		s.runFrames(3);

		check(s.faulted(), "the MCU fault is STICKY");
		checkFaultSurface(s, 0, g2::JobFault::CoreHalted, "MCU, three quanta later");

		/* reset() clears the Board's own bit too -- otherwise the very next
		 * quantum would re-latch from a machine reset() claimed to have
		 * cleared. */
		s.reset();

		check(!s.faulted(), "reset() clears the MCU fault");
		check(!board.faulted(), "reset() cleared the Board's own fault bit");
		checkFaultSurface(s, kDspCount + 1u, g2::JobFault::None, "MCU, after reset");
	}

	if(g_failures != 0)
	{
		std::printf("t0_scheduler_faults: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_scheduler_faults: all %d cases passed\n", g_cases);
	return 0;
}
