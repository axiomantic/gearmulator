/* t0_mcu_debt.cpp -- the check of SCH-21 step 6 (formerly SCH-30). Design
 * sections 13.4.1, 13.4.6, 13.10.5.
 *
 * WHAT THIS FILE ASSERTS, AND WHY IT COULD NOT BE WRITTEN BEFORE. Design
 * section 13.4.6's cycle-debt block carries `spent - want` forward. Until
 * `mcf5307_exec` stopped clamping its return to its budget, `spent <= want`
 * held on every call, the difference was never positive, the floor at zero
 * turned it into zero, and `cycleDebt(0)` was identically zero for every
 * implementation -- correct or pinned. Section 24.6 row W3-410 records that,
 * and records that this target was WITHDRAWN rather than shipped green over a
 * clause it could not test. The core now reports the whole cost of the
 * instruction that crossed the budget, so the overshoot is real and the debt
 * accrues. CASE 1 BELOW RE-ESTABLISHES THAT PROPERTY FROM THE LINKED LIBRARY
 * RATHER THAN TRUSTING IT, because every later case is vacuous without it.
 *
 * NO CYCLE COST IS TYPED ANYWHERE IN THIS FILE. The cost of the one
 * instruction the fixture runs is MEASURED from the linked core in case 1, and
 * every rate, every expected spend and the rule 2 bound are DERIVED from that
 * measurement. Section 24.6 records what a typed count costs: `dspJob`
 * hardcoded 8 where the real count was 30.
 *
 * THE INDEPENDENT OBSERVABLE IS THE PROGRAM COUNTER, AND THAT IS THE WHOLE
 * REASON THE FIXTURE IS A FIELD OF ONE REPEATED INSTRUCTION. `Board::runMcu`
 * returns the cycles it spent, and the Scheduler swallows that return -- so a
 * check that read the debt alone would be reading the accounting against
 * itself. The pc advance is machine progress that no accounting produces: the
 * number of instructions retired is `(pc after - pc before) / bytesPerInstr`,
 * and the cycles they cost is that count times the MEASURED cost. Every
 * per-quantum assertion below holds the Scheduler's `cycleDebt(0)` against
 * that figure.
 *
 * THE TWO DRIFT WORKLOADS ARE SECTION 13.4.6's AND BOTH ARE DERIVED FROM THE
 * MEASURED COST `c`.
 *
 *   NEVER-IDLE, at rate (4c - 1)/1. Four instructions cost 4c and the budget
 *   is one cycle short of it, so every quantum overruns by a little and the
 *   `want <= 0` branch never fires. The debt walks 0, 1, 2, ... c-1 and
 *   returns to zero, for ever, with no drift.
 *
 *   FORCED-IDLE, at rate 1/1. One instruction costs c and the budget is 1, so
 *   the first quantum overruns by c-1 and the next c-1 quanta run NOTHING and
 *   pay the debt down by one whole allocation each. That is rule 4's long
 *   dispatch, driven by the real core.
 *
 * A SPEC DISAGREEMENT IS RECORDED HERE RATHER THAN RESOLVED SILENTLY. Step
 * 6's check 2 reads "`cycleDebt(0)` and `longDispatchQuanta(0)` move under
 * BOTH drift workloads". `longDispatchQuanta` counts the `want <= 0` branch,
 * and "never-idle" is section 13.4.6's name for the workload in which that
 * branch never fires -- so the two clauses contradict each other for the
 * never-idle half. THIS FILE ASSERTS THE STRONGER READING AND NOT THE WEAKER:
 * under the never-idle workload `cycleDebt(0)` moves and `longDispatchQuanta(0)`
 * is asserted EXACTLY ZERO, which is a falsifiable claim about that branch
 * rather than a clause dropped; under the forced-idle workload both move.
 *
 * EVERY "IT MOVED" CLAIM IS GUARDED BEFORE ANY EQUALITY IS COMPARED. Case 2
 * requires the debt to take at least two distinct values and to reach a
 * non-zero one before case 4 compares the Scheduler's sequence against the
 * shared block's; otherwise "identical" would be a comparison of zero against
 * zero, which is the shape section 92 names.
 *
 * NOTHING HERE IS A LANGUAGE assert() AND NOTHING CATCHES AN EXCEPTION. Every
 * verdict is the failure counter, which no build type removes. The compile-time
 * half is `static_assert`, which the default build keeps.
 */

#include "board.h"
#include "cycleDebt.h"
#include "executor.h"
#include "mcuContext.h"
#include "memoryMap.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <type_traits>
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

	void checkEqual(const int64_t _observed, const int64_t _expected, const char* const _what)
	{
		++g_cases;

		if(_observed == _expected)
			return;

		std::printf("FAIL %s: observed %lld, expected %lld\n", _what,
			static_cast<long long>(_observed), static_cast<long long>(_expected));
		++g_failures;
	}

	/* THE FOUR MEMBERS THE SHARED BLOCK READS, HELD AT COMPILE TIME. The one
	 * block of design section 13.4.6 is a template and instantiates against
	 * `McuContext` and `DspContext` alike only because both carry these four
	 * names with these four types. A rename or a re-type here is a compile
	 * error at this line rather than a second block nobody can diff. */
	static_assert(std::is_same<decltype(g2::McuContext::rate),               ::Rational>::value,
		"McuContext::rate is the section 13.4.1 rational the shared block reads");
	static_assert(std::is_same<decltype(g2::McuContext::acc),                uint32_t>::value,
		"McuContext::acc is the section 13.4.1 accumulator the shared block reads");
	static_assert(std::is_same<decltype(g2::McuContext::debt),               int64_t>::value,
		"McuContext::debt is the signed section 13.4.6 debt the shared block reads");
	static_assert(std::is_same<decltype(g2::McuContext::longDispatchQuanta), uint64_t>::value,
		"McuContext::longDispatchQuanta is the rule 4 counter the shared block reads");

	/* Byte-addressed big-endian memory, the shape t0_board_mcu_handle uses. It
	 * keeps no access record: every assertion in this file is about the core's
	 * progress and about the Scheduler's accounting. */
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const uint32_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);

			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			uint32_t value = 0u;

			for(int i = 0; i < count; ++i)
				value = (value << 8) | uint32_t(m_bytes[_offset + uint32_t(i)]);

			_status = MCF5307_BUS_OK;
			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value,
			mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);

			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}

			for(int i = 0; i < count; ++i)
			{
				const int shift = 8 * (count - 1 - i);
				m_bytes[_offset + uint32_t(i)] = uint8_t((_value >> shift) & 0xffu);
			}

			_status = MCF5307_BUS_OK;
		}

		void pokeWord(const uint32_t _offset, const uint16_t _value)
		{
			m_bytes[_offset]      = uint8_t(_value >> 8);
			m_bytes[_offset + 1u] = uint8_t(_value & 0xffu);
		}

	private:
		static int byteCount(const int _sizeBits)
		{
			switch(_sizeBits)
			{
			case 8:  return 1;
			case 16: return 2;
			case 32: return 4;
			default: return 0;
			}
		}

		std::vector<uint8_t> m_bytes;
	};

	/* NOP. It is the instruction CPU-12's own tests use for a field the core
	 * runs through without halting and without faulting, and it is the ONE
	 * instruction this fixture contains -- which is what makes "the cost of the
	 * longest single dispatch unit this context's backend can issue" a figure
	 * this file can MEASURE rather than one section 1.3 rule 1 forbids it to
	 * invent. */
	constexpr uint16_t g_nop = 0x4E71u;

	constexpr uint32_t g_windowBase = g2::g_sdramBase;
	constexpr uint32_t g_windowSize = 0x1000u;
	constexpr uint32_t g_stackTop   = g_windowBase + g_windowSize;
	constexpr uint32_t g_codeBase   = g_windowBase;

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;
		config.memory.sdram = { g_windowBase, g_windowSize };
		return config;
	}

	void fillWithNops(Ram& _ram)
	{
		for(uint32_t offset = 0; offset + 1u < g_windowSize; offset += 2u)
			_ram.pokeWord(offset, g_nop);
	}

	/* ONE MACHINE: a Board with an SDRAM window full of NOPs and a core reset
	 * into it. The Ram must outlive the Board's use of it, so both live here
	 * and the declaration order is the lifetime. */
	struct Machine
	{
		Ram      ram{ g_windowSize };
		g2::Board board{ makeConfig() };

		Machine()
		{
			fillWithNops(ram);
			board.memory().attach(g2::Region::Sdram, &ram);
			board.resetMcu(g_stackTop, g_codeBase);
		}

		uint32_t pc() const { return board.mcuReg(17); }
	};

	/* One quantum's observation. Everything here is read from outside the
	 * Scheduler: the pc from the Board, the two counters through the section
	 * 13.10.5 accessors at index 0. */
	struct Observation
	{
		int64_t  budget      = 0;
		int64_t  debtBefore  = 0;
		int64_t  debtAfter   = 0;
		uint64_t ldqBefore   = 0;
		uint64_t ldqAfter    = 0;
		int64_t  spent       = 0;   /* derived from the pc advance */
	};
}

int main()
{
	std::printf("t0_mcu_debt: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	/* ---------------------------------------------------------------------
	 * CASE 1. THE MEASUREMENT, AND THE PROPERTY EVERY LATER CASE RESTS ON.
	 *
	 * A budget of ONE cycle is offered to the linked core. It cannot abandon
	 * an instruction it has started, so it retires exactly one instruction and
	 * reports that instruction's WHOLE cost. A core that clamped its return to
	 * its budget would answer 1, `spent <= want` would hold on every call for
	 * ever, and design section 13.4.6's floor at zero would make `cycleDebt(0)`
	 * identically zero -- which is section 24.6 row W3-410's finding and the
	 * reason this target was withdrawn. THIS CASE IS THE PROOF THAT THE
	 * LIBRARY THIS BINARY LINKS IS THE ONE THAT REPORTS THE OVERSHOOT.
	 */
	Machine probe;

	const uint32_t probePc0   = probe.pc();
	const uint32_t probeSpent = probe.board.runMcu(1u);
	const uint32_t probePc1   = probe.pc();

	check(!probe.board.mcuHalted(), "case 1: the NOP field runs without halting the core");
	check(!probe.board.faulted(),   "case 1: the NOP field runs without faulting the core");
	check(probePc1 > probePc0,      "case 1: a budget of one cycle retired an instruction");

	const int64_t bytesPerInstr = static_cast<int64_t>(probePc1) - static_cast<int64_t>(probePc0);
	const int64_t instrCost     = static_cast<int64_t>(probeSpent);

	check(probeSpent > 1u,
		"case 1: a budget of one cycle reported MORE than one cycle -- the linked mcf5307 "
		"reports the whole cost of the instruction that crossed the budget, so the cycle "
		"debt can accrue at all");

	if(bytesPerInstr <= 0 || instrCost <= 1)
	{
		/* Every later case derives its rates and its expected spends from these
		 * two figures. Running them against a clamped or a stalled core would
		 * report failures whose cause is this measurement, so the file stops
		 * here and says which figure is wrong. */
		std::printf("t0_mcu_debt: the fixture measurement is unusable "
			"(bytes/instruction %lld, cycles/instruction %lld)\n",
			static_cast<long long>(bytesPerInstr), static_cast<long long>(instrCost));
		std::printf("t0_mcu_debt: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_mcu_debt: MEASURED %lld byte(s) and %lld cycle(s) for one dispatch unit\n",
		static_cast<long long>(bytesPerInstr), static_cast<long long>(instrCost));

	if(!dsp56k::g_useJIT)
	{
		/* In an interpreter build no Scheduler can be created at all (section
		 * 11.4.3), so the refusal is the only claim the rest of this file may
		 * make. Case 1 above needs no Scheduler and has already run. */
		g2::Board          board;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;
		config.backend = g2::Backend::Jit;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, board, status);

		check(scheduler == nullptr, "an interpreter build yields no Scheduler");
		checkEqual(static_cast<int64_t>(status), static_cast<int64_t>(g2::Status::BadBackend),
			"an interpreter build reports BadBackend");

		std::printf("t0_mcu_debt: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return g_failures == 0 ? 0 : 1;
	}

	/* THE TWO WORKLOADS' RATES, DERIVED. Neither numerator is written down as a
	 * figure: both are functions of the cost case 1 measured. */
	const ::Rational neverIdleRate  = { static_cast<uint32_t>(4 * instrCost - 1), 1u };
	const ::Rational forcedIdleRate = { 1u, 1u };

	/* Runs `_quanta` quanta ONE AT A TIME against a fresh machine at `_rate`,
	 * recording the section 13.10.5 accessors and the pc advance around each.
	 * Returns false when the fixture stopped being a NOP field, which is a
	 * failure of the fixture rather than of the code under test and is reported
	 * as its own case. */
	const auto drive = [&](const ::Rational _rate, const size_t _quanta, const char* const _name,
		std::vector<Observation>& _out) -> bool
	{
		Machine            machine;
		g2::SerialExecutor executor;

		g2::Scheduler::Config config;
		config.mcuRate = _rate;

		g2::Status status{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(config, executor, machine.board, status);

		char what[256];

		std::snprintf(what, sizeof(what), "%s: the Config yields a Scheduler", _name);
		check(scheduler != nullptr, what);

		if(!scheduler)
			return false;

		g2::Scheduler& s = *scheduler;

		/* The MIRROR accumulator. The budget of each quantum is taken from the
		 * SAME `alloc()` the block uses, against a private accumulator seeded
		 * exactly as `McuContext::acc` is at construction, so the expected
		 * budget is computed by design section 13.4.1's own function rather
		 * than written here as a figure. */
		uint32_t mirrorAcc = 0;

		for(size_t q = 0; q < _quanta; ++q)
		{
			Observation o;

			o.budget     = static_cast<int64_t>(::alloc(_rate, &mirrorAcc));
			o.debtBefore = s.cycleDebt(0);
			o.ldqBefore  = s.longDispatchQuanta(0);

			const uint32_t pcBefore = machine.pc();

			s.runFrames(1);

			const uint32_t pcAfter = machine.pc();

			o.debtAfter = s.cycleDebt(0);
			o.ldqAfter  = s.longDispatchQuanta(0);

			if(pcAfter < pcBefore || pcAfter >= g_windowBase + g_windowSize ||
				machine.board.mcuHalted() || machine.board.faulted())
			{
				std::snprintf(what, sizeof(what),
					"%s: the core stayed inside the NOP field, unhalted and unfaulted, "
					"through quantum %zu", _name, q);
				check(false, what);
				return false;
			}

			const int64_t bytes = static_cast<int64_t>(pcAfter) - static_cast<int64_t>(pcBefore);

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: the pc advanced by a whole number of dispatch units", _name, q);
			checkEqual(bytes % bytesPerInstr, 0, what);

			o.spent = (bytes / bytesPerInstr) * instrCost;

			_out.push_back(o);
		}

		return true;
	};

	/* Holds every observation of one workload against design section 13.4.6's
	 * block, quantum by quantum. THE EXPECTED VALUES ARE THE BLOCK'S OWN
	 * ARITHMETIC APPLIED TO THE MEASURED SPEND, so a Scheduler that pinned
	 * either accessor, dropped the carry, forgot the floor at zero or paid the
	 * idle branch down twice reports here. */
	/* REPORTED, NOT ASSERTED. The four figures of each quantum, so that a
	 * reader of a passing run can see the debt walk rather than take it on
	 * trust, and so that a failing run carries its own context. Nothing here is
	 * a verdict; every verdict is the failure counter. */
	const auto report = [&](const std::vector<Observation>& _obs, const char* const _name)
	{
		std::printf("t0_mcu_debt: %s -- quantum: budget want spent debt ldq\n", _name);

		for(size_t q = 0; q < _obs.size(); ++q)
		{
			std::printf("t0_mcu_debt:   %s q%zu: %lld %lld %lld %lld %llu\n", _name, q,
				static_cast<long long>(_obs[q].budget),
				static_cast<long long>(_obs[q].budget - _obs[q].debtBefore),
				static_cast<long long>(_obs[q].spent),
				static_cast<long long>(_obs[q].debtAfter),
				static_cast<unsigned long long>(_obs[q].ldqAfter));
		}
	};

	const auto holdAgainstTheBlock = [&](const std::vector<Observation>& _obs, const char* const _name)
	{
		for(size_t q = 0; q < _obs.size(); ++q)
		{
			const Observation& o    = _obs[q];
			const int64_t      want = o.budget - o.debtBefore;

			char what[256];

			/* RULE 2's BOUND, AT EVERY QUANTUM BOUNDARY, AGAINST THE FINITE CAP
			 * THIS FIXTURE SUPPLIES. `instrCost` is the cost of the longest --
			 * and only -- dispatch unit the NOP field can issue, so it is the
			 * `maxDispatchCost` of design section 13.4.6's invariant for THIS
			 * context, measured rather than invented. */
			std::snprintf(what, sizeof(what),
				"%s quantum %zu: 0 <= debt (%lld) < one dispatch unit (%lld) at the boundary",
				_name, q, static_cast<long long>(o.debtAfter), static_cast<long long>(instrCost));
			check(o.debtAfter >= 0 && o.debtAfter < instrCost, what);

			if(want <= 0)
			{
				/* Rule 4's branch: nothing runs, the debt is paid down by ONE
				 * whole allocation and the counter rises by EXACTLY one. */
				std::snprintf(what, sizeof(what),
					"%s quantum %zu: the want <= 0 branch ran no instruction", _name, q);
				checkEqual(o.spent, 0, what);

				std::snprintf(what, sizeof(what),
					"%s quantum %zu: the want <= 0 branch paid the debt down by one whole "
					"allocation", _name, q);
				checkEqual(o.debtAfter, o.debtBefore - o.budget, what);

				std::snprintf(what, sizeof(what),
					"%s quantum %zu: the want <= 0 branch raised longDispatchQuanta(0) by "
					"exactly one", _name, q);
				checkEqual(static_cast<int64_t>(o.ldqAfter - o.ldqBefore), 1, what);

				continue;
			}

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: a running quantum did not count a long dispatch", _name, q);
			checkEqual(static_cast<int64_t>(o.ldqAfter - o.ldqBefore), 0, what);

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: the core was asked for %lld cycle(s) and ran at least that "
				"many (%lld)", _name, q, static_cast<long long>(want),
				static_cast<long long>(o.spent));
			check(o.spent >= want, what);

			const int64_t carried = o.spent - want;

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: the debt carried is spent - want, floored at zero", _name, q);
			checkEqual(o.debtAfter, carried > 0 ? carried : 0, what);
		}
	};

	/* Replays the SHARED BLOCK ITSELF -- g2::runQuantum from cycleDebt.h,
	 * against a real McuContext -- driven by the spends the Board actually
	 * produced, and asserts the resulting sequence equals the Scheduler's,
	 * quantum by quantum. THAT IS WHAT "ONE BLOCK USED TWICE" MEANS AS AN
	 * OBSERVABLE: a second block that resembled the first would have to agree
	 * with it on every quantum of a sequence that moves, and this file requires
	 * the sequence to move before it compares. */
	const auto holdAgainstTheSharedBlock = [&](const std::vector<Observation>& _obs,
		const ::Rational _rate, const char* const _name)
	{
		g2::McuContext ctx{ _rate, 0, 0, 0 };

		size_t runs = 0;

		for(size_t q = 0; q < _obs.size(); ++q)
		{
			const Observation& o = _obs[q];

			char what[256];

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: the shared block's debt equals the Scheduler's before the "
				"quantum", _name, q);
			checkEqual(ctx.debt, o.debtBefore, what);

			const int64_t returned = g2::runQuantum(ctx, [&](const uint32_t _want) -> uint32_t
			{
				++runs;
				(void) _want;
				return static_cast<uint32_t>(o.spent);
			});

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: the shared block's debt equals the Scheduler's after the "
				"quantum", _name, q);
			checkEqual(ctx.debt, o.debtAfter, what);

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: the shared block's longDispatchQuanta equals the Scheduler's",
				_name, q);
			checkEqual(static_cast<int64_t>(ctx.longDispatchQuanta),
				static_cast<int64_t>(o.ldqAfter), what);

			std::snprintf(what, sizeof(what),
				"%s quantum %zu: the shared block spent what the Board spent", _name, q);
			checkEqual(returned, o.spent, what);
		}

		/* The role-filler is invoked exactly once per RUNNING quantum and never
		 * in the want <= 0 branch. A mirror that never invoked it at all would
		 * otherwise agree with a Scheduler that never ran anything. */
		size_t running = 0;

		for(size_t q = 0; q < _obs.size(); ++q)
		{
			if(_obs[q].budget - _obs[q].debtBefore > 0)
				++running;
		}

		char what[256];

		std::snprintf(what, sizeof(what),
			"%s: the shared block invoked the role-filler once for each running quantum", _name);
		checkEqual(static_cast<int64_t>(runs), static_cast<int64_t>(running), what);
	};

	/* ---------------------------------------------------------------------
	 * CASE 2. THE NEVER-IDLE DRIFT WORKLOAD.
	 *
	 * The budget is one cycle short of four dispatch units, so every quantum
	 * overruns and the debt walks up to one unit and falls back. THE QUANTUM
	 * COUNT IS DERIVED, not chosen. The walk's period is one dispatch unit's
	 * worth of quanta and the debt after quantum q is (q + 1) mod that period,
	 * so THREE WHOLE PERIODS PLUS ONE runs several periods and ends on a debt
	 * of exactly one -- non-zero for EVERY measured cost, not merely for the
	 * one this machine happens to report. The non-zero ending is what makes
	 * case 5's conservation law able to see an accessor pinned to zero.
	 */
	const size_t neverIdleQuanta = static_cast<size_t>(instrCost) * 3u + 1u;

	std::vector<Observation> neverIdle;

	if(drive(neverIdleRate, neverIdleQuanta, "never-idle", neverIdle))
	{
		int64_t  maxDebt   = 0;
		int64_t  firstDebt = neverIdle.empty() ? 0 : neverIdle[0].debtAfter;
		bool     moved     = false;
		int64_t  totalSpent = 0;

		for(size_t q = 0; q < neverIdle.size(); ++q)
		{
			if(neverIdle[q].debtAfter > maxDebt)
				maxDebt = neverIdle[q].debtAfter;
			if(neverIdle[q].debtAfter != firstDebt)
				moved = true;
			totalSpent += neverIdle[q].spent;
		}

		/* THE ANTI-MIRAGE GUARDS, BEFORE ANY EQUALITY IS COMPARED. */
		check(totalSpent > 0, "never-idle: the core actually ran");
		check(maxDebt > 0,
			"never-idle: cycleDebt(0) reached a NON-ZERO value -- the clause section 24.6 row "
			"W3-410 recorded as structurally impossible before the core stopped clamping");
		check(moved,
			"never-idle: cycleDebt(0) took at least two distinct values across the run, so no "
			"later equality is a comparison of zero against zero");

		checkEqual(static_cast<int64_t>(neverIdle.back().ldqAfter), 0,
			"never-idle: longDispatchQuanta(0) is EXACTLY zero -- a never-idle context takes the "
			"want <= 0 branch no times, and this is the falsifiable form of step 6's check 2 for "
			"this workload");

		report(neverIdle, "never-idle");
		holdAgainstTheBlock(neverIdle, "never-idle");
		holdAgainstTheSharedBlock(neverIdle, neverIdleRate, "never-idle");

		/* ---------------------------------------------------------------
		 * CASE 5, on this workload. THE CONSERVATION LAW, WHICH IS
		 * INDEPENDENT OF EVERY PER-QUANTUM ASSERTION ABOVE.
		 *
		 * In the running branch the block gives debt' = spent - budget + debt,
		 * so summing over a run that never took the want <= 0 branch and
		 * never floored,
		 *
		 *     total spent  ==  total allocated  +  final debt.
		 *
		 * The left side is the pc's, the right side is the Config's and the
		 * accessor's. A `cycleDebt(0)` pinned to zero breaks it by exactly the
		 * debt it hid, which is why the quantum count above ends non-zero.
		 */
		int64_t totalAllocated = 0;

		for(size_t q = 0; q < neverIdle.size(); ++q)
			totalAllocated += neverIdle[q].budget;

		check(neverIdle.back().debtAfter > 0,
			"never-idle: the run ENDS on a non-zero debt, so the conservation law below can see "
			"an accessor pinned to zero");

		checkEqual(totalSpent, totalAllocated + neverIdle.back().debtAfter,
			"never-idle: the cycles the pc says ran equal the cycles allocated plus the debt "
			"still carried");
	}

	/* ---------------------------------------------------------------------
	 * CASE 3. THE FORCED-IDLE DRIFT WORKLOAD, AND RULE 4's COUNTER.
	 *
	 * The budget is one cycle and one dispatch unit costs `instrCost`, so one
	 * running quantum is followed by exactly `instrCost - 1` quanta that run
	 * NOTHING. Both accessors move here.
	 */
	const size_t forcedIdleQuanta = static_cast<size_t>(instrCost) * 3u;

	std::vector<Observation> forcedIdle;

	if(drive(forcedIdleRate, forcedIdleQuanta, "forced-idle", forcedIdle))
	{
		int64_t  maxDebt    = 0;
		int64_t  totalSpent = 0;
		bool     moved      = false;
		const int64_t firstDebt = forcedIdle.empty() ? 0 : forcedIdle[0].debtAfter;

		size_t idleQuanta = 0;

		for(size_t q = 0; q < forcedIdle.size(); ++q)
		{
			if(forcedIdle[q].debtAfter > maxDebt)
				maxDebt = forcedIdle[q].debtAfter;
			if(forcedIdle[q].debtAfter != firstDebt)
				moved = true;
			totalSpent += forcedIdle[q].spent;
			if(forcedIdle[q].ldqAfter != forcedIdle[q].ldqBefore)
				++idleQuanta;
		}

		check(totalSpent > 0, "forced-idle: the core actually ran");
		check(maxDebt > 0,    "forced-idle: cycleDebt(0) reached a NON-ZERO value");
		check(moved,          "forced-idle: cycleDebt(0) took at least two distinct values");

		check(forcedIdle.back().ldqAfter > 0,
			"forced-idle: longDispatchQuanta(0) rose above zero -- rule 4's branch fired, driven "
			"by the real core's overshoot");

		/* STEP 6's CHECK 3, ASSERTED AS AN EXACT EQUALITY AND NOT AS A BOUND:
		 * every quantum that took the branch raised the counter by exactly one,
		 * which `holdAgainstTheBlock` asserts individually, and the total is the
		 * number of such quanta. */
		checkEqual(static_cast<int64_t>(forcedIdle.back().ldqAfter),
			static_cast<int64_t>(idleQuanta),
			"forced-idle: longDispatchQuanta(0) counts exactly the quanta that took the "
			"want <= 0 branch, one each");

		check(idleQuanta > 0, "forced-idle: at least one long-dispatch quantum actually fired");

		report(forcedIdle, "forced-idle");
		holdAgainstTheBlock(forcedIdle, "forced-idle");
		holdAgainstTheSharedBlock(forcedIdle, forcedIdleRate, "forced-idle");

		/* CASE 4. THE DRIFT DIRECTION. Design section 13.4.6 says a forced-idle
		 * context drifts SLOW and never fast, bounded by one full allocation
		 * for each idle quantum. Slow means the machine ran no MORE than it was
		 * allocated, and the bound is what the idle quanta gave back. */
		int64_t totalAllocated = 0;

		for(size_t q = 0; q < forcedIdle.size(); ++q)
			totalAllocated += forcedIdle[q].budget;

		check(totalSpent <= totalAllocated + maxDebt,
			"forced-idle: the run drifts SLOW and never fast -- the cycles that ran never exceed "
			"the cycles allocated by more than the debt still carried");
	}

	std::printf("t0_mcu_debt: %d failure(s) in %d case(s)\n", g_failures, g_cases);
	return g_failures == 0 ? 0 : 1;
}
