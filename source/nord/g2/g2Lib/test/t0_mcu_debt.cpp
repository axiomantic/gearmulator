/* The cycle-debt block carries `spent - want` forward. A core that clamped its
 * return to its budget would make `spent <= want` hold on every call, the
 * difference would never be positive, the floor at zero would swallow it, and
 * `cycleDebt(0)` would be identically zero for every implementation, correct or
 * pinned. `mcf5307_exec` reports the whole cost of the instruction that crossed
 * the budget, so the overshoot is real and the debt accrues. Case 1 below
 * re-establishes that property from the linked library rather than trusting it,
 * because every later case is vacuous without it.
 *
 * No cycle cost is typed anywhere in this file. The cost of the one instruction
 * the fixture runs is measured from the linked core in case 1, and every rate,
 * every expected spend and the rule 2 bound are derived from that measurement.
 *
 * The independent observable is the program counter, which is why the fixture
 * is a field of one repeated instruction. `Board::runMcu` returns the cycles it
 * spent and the Scheduler swallows that return, so a check that read the debt
 * alone would be reading the accounting against itself. The pc advance is
 * machine progress that no accounting produces: the number of instructions
 * retired is `(pc after - pc before) / bytesPerInstr`, and the cycles they cost
 * is that count times the measured cost.
 *
 * The two drift workloads are both derived from the measured cost `c`.
 *
 *   Never-idle, at rate (4c - 1)/1. Four instructions cost 4c and the budget
 *   is one cycle short of it, so every quantum overruns by a little and the
 *   `want <= 0` branch never fires. The debt walks 0, 1, 2, ... c-1 and
 *   returns to zero, for ever, with no drift.
 *
 *   Forced-idle, at rate 1/1. One instruction costs c and the budget is 1, so
 *   the first quantum overruns by c-1 and the next c-1 quanta run nothing and
 *   pay the debt down by one whole allocation each. That is rule 4's long
 *   dispatch, driven by the real core.
 *
 * Under the never-idle workload `longDispatchQuanta(0)` is asserted exactly
 * zero rather than merely "moves": a never-idle context takes the `want <= 0`
 * branch no times, so the stronger claim is the falsifiable one.
 *
 * Every "it moved" claim is guarded before any equality is compared: the debt
 * must take at least two distinct values and reach a non-zero one, otherwise
 * "identical" would be a comparison of zero against zero.
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

	/* The members the shared block reads, held at compile time. The block is a
	 * template and instantiates against `McuContext` and `DspContext` alike
	 * only because both carry these names with these types. A rename or a
	 * re-type is a compile error at this line rather than a second block nobody
	 * can diff. */
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

	/* NOP: a field the core runs through without halting and without faulting,
	 * and the only instruction this fixture contains, which is what makes the
	 * cost of the longest single dispatch unit a figure this file can measure
	 * rather than invent. */
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

	/* A Board with an SDRAM window full of NOPs and a core reset into it. The
	 * Ram must outlive the Board's use of it, so both live here and the
	 * declaration order is the lifetime. */
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
	 * Scheduler: the pc from the Board, the two counters through the accessors
	 * at index 0. */
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

	/* The measurement, and the property every later case rests on.
	 *
	 * A budget of one cycle is offered to the linked core. It cannot abandon an
	 * instruction it has started, so it retires exactly one instruction and
	 * reports that instruction's whole cost. A core that clamped its return to
	 * its budget would answer 1, `spent <= want` would hold on every call for
	 * ever, and the floor at zero would make `cycleDebt(0)` identically zero.
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
		/* In an interpreter build no Scheduler can be created at all, so the
		 * refusal is the only claim the rest of this file may make. Case 1
		 * above needs no Scheduler and has already run. */
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

	/* Neither numerator is written down as a figure: both are functions of the
	 * cost case 1 measured. */
	const ::Rational neverIdleRate  = { static_cast<uint32_t>(4 * instrCost - 1), 1u };
	const ::Rational forcedIdleRate = { 1u, 1u };

	/* Runs `_quanta` quanta one at a time against a fresh machine at `_rate`,
	 * recording the accessors and the pc advance around each. Returns false
	 * when the fixture stopped being a NOP field, which is a failure of the
	 * fixture rather than of the code under test. */
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

		/* The mirror accumulator. The budget of each quantum is taken from the
		 * same `alloc()` the block uses, against a private accumulator seeded
		 * exactly as `McuContext::acc` is at construction, so the expected
		 * budget is computed rather than written here as a figure. */
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

	/* Reported, not asserted: the figures of each quantum, so that a reader of
	 * a passing run can see the debt walk and a failing run carries its own
	 * context. Every verdict is the failure counter. */
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

	/* Holds every observation of one workload against the block, quantum by
	 * quantum. The expected values are the block's own arithmetic applied to
	 * the measured spend, so a Scheduler that pinned either accessor, dropped
	 * the carry, forgot the floor at zero or paid the idle branch down twice
	 * reports here. */
	const auto holdAgainstTheBlock = [&](const std::vector<Observation>& _obs, const char* const _name)
	{
		for(size_t q = 0; q < _obs.size(); ++q)
		{
			const Observation& o    = _obs[q];
			const int64_t      want = o.budget - o.debtBefore;

			char what[256];

			/* `instrCost` is the cost of the longest, and only, dispatch unit
			 * the NOP field can issue, so it is the `maxDispatchCost` of the
			 * invariant for this context, measured rather than invented. */
			std::snprintf(what, sizeof(what),
				"%s quantum %zu: 0 <= debt (%lld) < one dispatch unit (%lld) at the boundary",
				_name, q, static_cast<long long>(o.debtAfter), static_cast<long long>(instrCost));
			check(o.debtAfter >= 0 && o.debtAfter < instrCost, what);

			if(want <= 0)
			{
				/* Rule 4's branch: nothing runs, the debt is paid down by one
				 * whole allocation and the counter rises by exactly one. */
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

	/* Replays the shared block itself -- g2::runQuantum from cycleDebt.h,
	 * against a real McuContext -- driven by the spends the Board actually
	 * produced, and asserts the resulting sequence equals the Scheduler's,
	 * quantum by quantum. A second block that resembled the first would have to
	 * agree with it on every quantum of a sequence that moves, and this file
	 * requires the sequence to move before it compares. */
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

		/* The role-filler is invoked exactly once per running quantum and never
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

	/* The never-idle drift workload. The budget is one cycle short of four
	 * dispatch units, so every quantum overruns and the debt walks up to one
	 * unit and falls back. The quantum count is derived, not chosen: the walk's
	 * period is one dispatch unit's worth of quanta and the debt after quantum
	 * q is (q + 1) mod that period, so three whole periods plus one ends on a
	 * debt of exactly one, non-zero for every measured cost rather than for the
	 * one this machine happens to report. The non-zero ending is what lets the
	 * conservation law below see an accessor pinned to zero.
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

		/* The guards, before any equality is compared. */
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

		/* The conservation law, which is independent of every per-quantum
		 * assertion above.
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

	/* The forced-idle drift workload, and rule 4's counter. The budget is one
	 * cycle and one dispatch unit costs `instrCost`, so one running quantum is
	 * followed by exactly `instrCost - 1` quanta that run nothing. Both
	 * accessors move here.
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

		/* An exact equality and not a bound: every quantum that took the branch
		 * raised the counter by exactly one, which `holdAgainstTheBlock`
		 * asserts individually, and the total is the number of such quanta. */
		checkEqual(static_cast<int64_t>(forcedIdle.back().ldqAfter),
			static_cast<int64_t>(idleQuanta),
			"forced-idle: longDispatchQuanta(0) counts exactly the quanta that took the "
			"want <= 0 branch, one each");

		check(idleQuanta > 0, "forced-idle: at least one long-dispatch quantum actually fired");

		report(forcedIdle, "forced-idle");
		holdAgainstTheBlock(forcedIdle, "forced-idle");
		holdAgainstTheSharedBlock(forcedIdle, forcedIdleRate, "forced-idle");

		/* A forced-idle context drifts slow and never fast, bounded by one full
		 * allocation for each idle quantum: the machine ran no more than it was
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
