/* t0_scheduler_state.cpp -- the check of SCH-21 step 4 (the absorbed SCH-24).
 * Design sections 13.10 rule 2, 13.10.5.
 *
 * THE ROW, VERBATIM: "Run 100 quanta, save, run 100 more, load, run the same
 * 100 again, and assert identical output. Assert identical cycleDebt(i),
 * identical longDispatchQuanta(i) and identical rational accumulator for every
 * i in 0 .. dspCount, index 0 included. Assert the version word round-trips and
 * that a perturbed version word is a named load failure rather than a silent
 * acceptance."
 *
 * WHY THIS FILE COULD NOT BE WRITTEN BEFORE, AND WHAT CHANGED. Section 24.6 row
 * W3-415 recorded the blocker: the Board's constructor always calls
 * attachHdi08Bridges, no detach existed anywhere in source/nord/g2, and
 * DspSet::stateLoad answers Status::BridgesAttached before its first write
 * whenever the set holds bridges -- so a Scheduler::stateLoad that composes the
 * DSP set could never return Ok on any Scheduler built from a real Board. The
 * operator ruled for the detach. DspSet now carries detachHdi08Bridges and its
 * exact inverse reattachHdi08Bridges, and Scheduler::stateLoad brackets the DSP
 * limb with the pair.
 *
 * THE PAIR IS A HAZARD AND IT HAS ITS OWN CASE. A detach and a re-attach that
 * are not exactly inverse would hide behind a green round trip: every borrowed
 * programLanded pointer in the Scheduler's own contexts is an address INTO a
 * bridge, so a re-attach that rebuilt the bridges, reordered them or dropped one
 * would leave the run gate reading a dangling or a foreign flag while every
 * state comparison in this file still matched. CASE 2 pins the pair by the
 * bridge IDENTITY at each index -- the pointer value programLanded(i) answers --
 * and not by a count.
 *
 * THE MIRAGE THIS TARGET IS UNIQUELY EXPOSED TO, AND WHAT IS DONE ABOUT IT. In
 * a T0 fixture no firmware lands, every DSP run gate is shut, and DSP memory
 * never changes -- so a round trip that compared DSP state would be comparing a
 * zero with a zero and would pass against an implementation that saved nothing.
 * Three separate defences, and each one is asserted BEFORE the equality it
 * guards:
 *
 *   1. THE STATE MOVES. The digest at the save point and the digest 100 quanta
 *      later must DIFFER, and the two images must differ byte-wise. The MCU
 *      context is what moves it: the fixture is a field of one repeated
 *      instruction, the core reports the whole cost of the instruction that
 *      crossed its budget, so the section 13.4.6 debt accrues and the section
 *      13.4.1 accumulator walks its denominator.
 *   2. THE LOAD PUTS THE MACHINE BACK. The digest immediately after the load
 *      must equal the digest AT THE SAVE POINT, which no zero-byte snapshot and
 *      no stateLoad that returns Ok without loading can satisfy -- both leave
 *      the digest where the second hundred quanta left it.
 *   3. THE DSP LIMB IS DRIVEN BY HAND. Case 5 writes a distinct generation into
 *      every slot's registers and into P, X and Y, saves through the SCHEDULER,
 *      writes a second generation, loads through the Scheduler, and reads the
 *      first generation back. Nothing in that case is a zero.
 *
 * WHAT "IDENTICAL OUTPUT" IS HERE, STATED RATHER THAN ASSUMED, AND IT IS THIS
 * FILE'S ONE DEPARTURE FROM THE ROW'S WORDING. The row's "output" cannot be the
 * frames pull() answers: the codec source injects into audio mailbox 0, the
 * codec sink is extracted from the tail mailbox, and NOTHING carries a frame
 * between them but the eight DSPs -- which cannot run without firmware. Every
 * pulled frame in a T0 fixture is a zero frame whatever the Scheduler does, so
 * an equality over them is the exact 0 == 0 the row exists to forbid. The output
 * asserted here is instead the WHOLE observable state the hundred quanta
 * produce: the section 13.10.5 accessor surface, digest by digest, AND the
 * snapshot image byte for byte. The image is what carries the rational
 * accumulators, which the class exposes through no accessor.
 *
 * THE ACCUMULATOR EQUALITY IS THE IMAGE EQUALITY. Scheduler carries no
 * accessor for McuContext::acc or DspContext::acc and this file adds none:
 * scheduler.h's public surface is the thread map's census and a new accessor
 * would move it. The image holds every accumulator, so a byte-identical image
 * is a stronger statement than nine accessor comparisons would be.
 *
 * NOTHING HERE IS A LANGUAGE assert() AND NOTHING CATCHES AN EXCEPTION. Every
 * verdict is the failure counter, which no build type removes; the compile-time
 * half is static_assert, which fires in every build type. The default build is
 * Release and Release defines NDEBUG.
 *
 * NO COUNT IS TYPED. The DSP count comes from the set, the context count from
 * the set, the image sizes from the objects, and the instruction cost from the
 * linked core.
 */

#include "board.h"
#include "chainAdapter.h"
#include "dspSet.h"
#include "executor.h"
#include "memoryMap.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;

		if(_condition)
			return;

		std::printf("FAIL %s\n", _what.c_str());
		++g_failures;
	}

	void checkEqualI64(const int64_t _observed, const int64_t _expected, const std::string& _what)
	{
		++g_cases;

		if(_observed == _expected)
			return;

		std::printf("FAIL %s: observed %lld, expected %lld\n", _what.c_str(),
			static_cast<long long>(_observed), static_cast<long long>(_expected));
		++g_failures;
	}

	std::string atIndex(const unsigned _i)
	{
		return " [index " + std::to_string(_i) + "]";
	}

	/* ---------------------------------------------------------------------
	 * THE SURFACE, HELD AT COMPILE TIME.
	 *
	 * The three Scheduler methods are `noexcept` and carry the row's return
	 * types. `stateLoad` returns g2::Status and not void: design section 13.10
	 * rule 2 forbids an exception, a release build removes an assertion, and a
	 * void return leaves "a named load failure" with no channel at all.
	 *
	 * THE THREE COMPOSED LIMBS ARE PINNED HERE TOO, because the composition can
	 * only report "the first non-Ok status any of the three produces" if all
	 * three HAVE a status to produce. Board::stateLoad and
	 * ChainAdapter::stateLoad returned void until this task reconciled them,
	 * which is the correction design section 13.10.5 records.
	 */
	static_assert(noexcept(std::declval<const g2::Scheduler&>().stateSize()),
		"Scheduler::stateSize must be noexcept");
	static_assert(noexcept(std::declval<const g2::Scheduler&>().stateSave(nullptr)),
		"Scheduler::stateSave must be noexcept");
	static_assert(noexcept(std::declval<g2::Scheduler&>().stateLoad(nullptr)),
		"Scheduler::stateLoad must be noexcept");

	static_assert(std::is_same_v<decltype(std::declval<const g2::Scheduler&>().stateSize()), size_t>,
		"Scheduler::stateSize returns the byte count of a flat block");
	static_assert(std::is_same_v<decltype(std::declval<const g2::Scheduler&>().stateSave(nullptr)), void>,
		"Scheduler::stateSave returns void");
	static_assert(std::is_same_v<decltype(std::declval<g2::Scheduler&>().stateLoad(nullptr)), g2::Status>,
		"Scheduler::stateLoad reports through g2::Status");

	static_assert(std::is_same_v<decltype(std::declval<g2::Board&>().stateLoad(nullptr)), g2::Status>,
		"Board::stateLoad is reconciled to g2::Status");
	static_assert(std::is_same_v<decltype(std::declval<g2::ChainAdapter&>().stateLoad(nullptr)), g2::Status>,
		"ChainAdapter::stateLoad is reconciled to g2::Status");
	static_assert(std::is_same_v<decltype(std::declval<g2::DspSet&>().stateLoad(nullptr)), g2::Status>,
		"DspSet::stateLoad already reports through g2::Status");

	/* THE DETACH AND ITS INVERSE, AND THE OBSERVABLE THAT SEPARATES THEM. */
	static_assert(noexcept(std::declval<g2::DspSet&>().detachHdi08Bridges()),
		"DspSet::detachHdi08Bridges must be noexcept");
	static_assert(noexcept(std::declval<g2::DspSet&>().reattachHdi08Bridges()),
		"DspSet::reattachHdi08Bridges must be noexcept");
	static_assert(noexcept(std::declval<const g2::DspSet&>().bridgesAttached()),
		"DspSet::bridgesAttached must be noexcept");
	static_assert(std::is_same_v<decltype(std::declval<const g2::DspSet&>().bridgesAttached()), bool>,
		"DspSet::bridgesAttached answers a bool");

	/* ---------------------------------------------------------------------
	 * THE FIXTURE. A Board with an SDRAM window full of one repeated
	 * instruction and a core reset into it -- the machine t0_mcu_debt drives,
	 * for the same reason: it is the ONE part of a T0 Scheduler whose emulated
	 * state moves, and a round trip over a machine whose state never moves is
	 * satisfied by a snapshot of zero bytes.
	 */
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

	/* NOP. The one instruction the field holds, so "the cost of one dispatch
	 * unit" is a figure this file MEASURES from the linked core rather than one
	 * it writes down. */
	constexpr uint16_t g_nop = 0x4E71u;

	constexpr uint32_t g_windowBase = g2::g_sdramBase;
	constexpr uint32_t g_windowSize = 0x4000u;
	constexpr uint32_t g_stackTop   = g_windowBase + g_windowSize;
	constexpr uint32_t g_codeBase   = g_windowBase;

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;
		config.memory.sdram = { g_windowBase, g_windowSize };
		return config;
	}

	struct Machine
	{
		Ram       ram{ g_windowSize };
		g2::Board board{ makeConfig() };

		Machine()
		{
			for(uint32_t offset = 0; offset + 1u < g_windowSize; offset += 2u)
				ram.pokeWord(offset, g_nop);

			board.memory().attach(g2::Region::Sdram, &ram);
			board.resetMcu(g_stackTop, g_codeBase);
		}

		uint32_t pc() const { return board.mcuReg(17); }
	};

	/* THE WHOLE OBSERVABLE STATE OF ONE Scheduler, read through the design
	 * section 13.10.5 accessor surface and through nothing else. It is what
	 * this file means by "the output of a hundred quanta": the codec sink
	 * cannot carry one in a firmware-less fixture, and the file header states
	 * why at length. */
	struct Digest
	{
		uint64_t              frameIndex = 0;
		std::vector<uint64_t> underrun;
		std::vector<uint64_t> secondUnderrun;
		std::vector<uint64_t> phaseError;
		uint64_t              starved    = 0;
		uint64_t              overflow   = 0;
		uint64_t              dropped    = 0;
		uint64_t              underflow  = 0;
		std::vector<int64_t>  debt;
		std::vector<uint64_t> ldq;
		uint8_t               faulted    = 0;
		std::vector<uint32_t> fault;
	};

	Digest digestOf(const g2::Scheduler& _s, const unsigned _dspCount)
	{
		Digest d;

		d.frameIndex = _s.frameIndex();

		for(unsigned p = 0; p < _dspCount; ++p)
		{
			d.underrun.push_back(_s.underrunFrames(p));
			d.secondUnderrun.push_back(_s.secondBusUnderrunFrames(p));
			d.phaseError.push_back(_s.phaseErrorFrames(p));
		}

		d.starved   = _s.starvedFrames();
		d.overflow  = _s.overflowFrames();
		d.dropped   = _s.droppedFrames();
		d.underflow = _s.underflowFrames();

		/* INDEX 0 IS THE MCU AND 1 .. dspCount ARE THE DSPs, and index 0 is
		 * INCLUDED, which the row states in as many words. */
		for(unsigned i = 0; i <= _dspCount; ++i)
		{
			d.debt.push_back(_s.cycleDebt(i));
			d.ldq.push_back(_s.longDispatchQuanta(i));
			d.fault.push_back(static_cast<uint32_t>(_s.contextFault(i)));
		}

		d.faulted = _s.faulted() ? 1u : 0u;

		return d;
	}

	bool operator==(const Digest& _a, const Digest& _b)
	{
		return _a.frameIndex     == _b.frameIndex
			&& _a.underrun       == _b.underrun
			&& _a.secondUnderrun == _b.secondUnderrun
			&& _a.phaseError     == _b.phaseError
			&& _a.starved        == _b.starved
			&& _a.overflow       == _b.overflow
			&& _a.dropped        == _b.dropped
			&& _a.underflow      == _b.underflow
			&& _a.debt           == _b.debt
			&& _a.ldq            == _b.ldq
			&& _a.faulted        == _b.faulted
			&& _a.fault          == _b.fault;
	}

	/* A FAILING COMPARISON MUST SAY WHAT MOVED, or a red run costs a bisect. */
	void reportDigest(const Digest& _d, const char* const _name)
	{
		std::printf("t0_scheduler_state: %s -- frame %llu, debt(0) %lld, ldq(0) %llu, "
			"underrun(0) %llu, faulted %u\n", _name,
			static_cast<unsigned long long>(_d.frameIndex),
			static_cast<long long>(_d.debt.empty() ? 0 : _d.debt[0]),
			static_cast<unsigned long long>(_d.ldq.empty() ? 0 : _d.ldq[0]),
			static_cast<unsigned long long>(_d.underrun.empty() ? 0 : _d.underrun[0]),
			unsigned(_d.faulted));
	}

	/* THE FIRST DIFFERING BYTE, or the size of the block when they agree. */
	size_t firstDifference(const std::vector<uint8_t>& _a, const std::vector<uint8_t>& _b)
	{
		const size_t n = _a.size() < _b.size() ? _a.size() : _b.size();

		for(size_t i = 0; i < n; ++i)
		{
			if(_a[i] != _b[i])
				return i;
		}

		return _a.size() == _b.size() ? _a.size() : n;
	}

	void checkImagesEqual(const std::vector<uint8_t>& _a, const std::vector<uint8_t>& _b,
		const std::string& _what)
	{
		++g_cases;

		if(_a.size() == _b.size() && _a == _b)
			return;

		std::printf("FAIL %s: images differ (sizes %zu and %zu, first difference at byte %zu)\n",
			_what.c_str(), _a.size(), _b.size(), firstDifference(_a, _b));
		++g_failures;
	}

	void checkImagesDiffer(const std::vector<uint8_t>& _a, const std::vector<uint8_t>& _b,
		const std::string& _what)
	{
		++g_cases;

		if(_a.size() != _b.size() || _a != _b)
			return;

		std::printf("FAIL %s: the two images are identical, so nothing this snapshot covers "
			"moved and every equality below it is vacuous\n", _what.c_str());
		++g_failures;
	}

	std::vector<uint8_t> imageOf(const g2::Scheduler& _s)
	{
		std::vector<uint8_t> image(_s.stateSize(), 0u);

		if(!image.empty())
			_s.stateSave(image.data());

		return image;
	}

	/* ONE MACHINE AND ONE Scheduler OVER IT. THE DECLARATION ORDER IS THE
	 * LIFETIME: scheduler.h requires the Board to outlive the Scheduler, and a
	 * member declared later is destroyed first. */
	struct Rig
	{
		Machine                        machine;
		g2::SerialExecutor             executor;
		g2::Status                     status{};
		std::unique_ptr<g2::Scheduler> scheduler;

		explicit Rig(const ::Rational _mcuRate)
		{
			g2::Scheduler::Config config;
			config.mcuRate = _mcuRate;

			scheduler = g2::Scheduler::create(config, executor, machine.board, status);
		}
	};
}

int main()
{
	std::printf("t0_scheduler_state: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	/* =====================================================================
	 * CASE 1. THE DETACH IS A REAL DETACH AND THE RE-ATTACH IS ITS EXACT
	 * INVERSE.
	 *
	 * THE HAZARD, STATED BEFORE THE ASSERTIONS. Scheduler's constructor copies
	 * set.programLanded(i) into each DspContext and the run gate borrows that
	 * ADDRESS for the life of the object. A re-attach that constructed fresh
	 * bridges would leave every one of those borrowed pointers dangling; a
	 * re-attach that reordered them would point each gate at another slot's
	 * flag; a re-attach that dropped one would leave the last gate reading a
	 * null. NONE of the three is visible in a state comparison, which is why
	 * this case exists and why it pins the bridge IDENTITY at each index.
	 */
	{
		Machine machine;
		g2::DspSet& set = machine.board.dspSet();

		const unsigned dspCount = set.dspCount();

		check(dspCount > 0u, "case 1: the Board's DSP set holds at least one slot");
		check(set.bridgesAttached(),
			"case 1: a Board arrives with its HDI08 bridges attached -- board.cpp calls "
			"attachHdi08Bridges from the constructor body, unconditionally");

		/* THE IDENTITY OF EACH BRIDGE, taken through the one public reader of
		 * it. Nothing else in the tree can tell one bridge from another. */
		std::vector<const bool*> before(dspCount, nullptr);

		for(unsigned i = 0; i < dspCount; ++i)
		{
			before[i] = set.programLanded(i);
			check(before[i] != nullptr,
				"case 1: an attached set answers a landed flag for every slot" + atIndex(i));
		}

		for(unsigned i = 0; i < dspCount; ++i)
		{
			for(unsigned j = i + 1u; j < dspCount; ++j)
			{
				check(before[i] != before[j],
					"case 1: the slots answer DISTINCT flags, so an index comparison can "
					"tell one bridge from another" + atIndex(i) + atIndex(j));
			}
		}

		/* THE DETACH REACHES EVERY SLOT. A detach that emptied a count but left
		 * the flags readable would still let stateLoad refuse, and a detach
		 * that left one bridge behind would be invisible to a size check. */
		set.detachHdi08Bridges();

		check(!set.bridgesAttached(), "case 1: the detach leaves the set holding no bridges");

		for(unsigned i = 0; i < dspCount; ++i)
		{
			check(set.programLanded(i) == nullptr,
				"case 1: a detached set answers NO landed flag, which is the run gate's own "
				"reading of NOT LANDED" + atIndex(i));
		}

		/* THE DETACH IS WHAT UNBLOCKS THE LOAD, and this is where that is
		 * proven rather than assumed. Before the detach this same call answers
		 * BridgesAttached -- t0_dsp_boot_consumer pins that -- so an Ok here is
		 * the detach's own observable. */
		std::vector<uint8_t> dspImage(set.stateSize());
		set.stateSave(dspImage.data());

		check(set.stateLoad(dspImage.data()) == g2::Status::Ok,
			"case 1: a DETACHED set takes back the snapshot it wrote, which is the whole "
			"reason the detach exists");

		/* THE INVERSE. Same object at the same index, for every index. */
		set.reattachHdi08Bridges();

		check(set.bridgesAttached(), "case 1: the re-attach puts the bridges back");

		for(unsigned i = 0; i < dspCount; ++i)
		{
			check(set.programLanded(i) == before[i],
				"case 1: the re-attach restores THE SAME bridge at THE SAME index -- a "
				"re-attach that rebuilt, reordered or dropped one answers a different "
				"address here" + atIndex(i));
		}

		check(set.stateLoad(dspImage.data()) == g2::Status::BridgesAttached,
			"case 1: a re-attached set refuses a load again, so the re-attach restored the "
			"REFUSAL and not merely a container");

		/* THE PAIR IS REPEATABLE. attachHdi08Bridges refuses a second attach by
		 * throwing, so a re-attach implemented as a re-attach would survive one
		 * round and die on the second. */
		set.detachHdi08Bridges();
		set.reattachHdi08Bridges();

		check(set.bridgesAttached(), "case 1: a second detach and re-attach leaves the set attached");

		for(unsigned i = 0; i < dspCount; ++i)
		{
			check(set.programLanded(i) == before[i],
				"case 1: the second round restores the same bridge at the same index" + atIndex(i));
		}

		/* THE PAIR IS TOTAL AT BOTH ENDS. Neither call is allowed to be a
		 * one-shot that corrupts the set when it has nothing to do. */
		set.reattachHdi08Bridges();
		check(set.bridgesAttached(), "case 1: a re-attach of an attached set changes nothing");

		set.detachHdi08Bridges();
		set.detachHdi08Bridges();
		check(!set.bridgesAttached(), "case 1: a second detach of a detached set changes nothing");

		set.reattachHdi08Bridges();

		for(unsigned i = 0; i < dspCount; ++i)
		{
			check(set.programLanded(i) == before[i],
				"case 1: the set survives the redundant calls with its bridges intact" + atIndex(i));
		}
	}


	/* =====================================================================
	 * THE FIXTURE MEASUREMENT, AND EVERY LATER CASE RESTS ON IT.
	 *
	 * A budget of ONE cycle is offered to the linked core. It cannot abandon an
	 * instruction it has started, so it retires exactly one and reports that
	 * instruction's WHOLE cost. t0_mcu_debt carries the same measurement and the
	 * reason it is a measurement rather than a constant: a core that clamped its
	 * return would make the section 13.4.6 debt identically zero, and the MCU
	 * context is the ONE part of a T0 Scheduler whose state moves.
	 */
	int64_t instrCost = 0;
	{
		Machine probe;

		const uint32_t pc0   = probe.pc();
		const uint32_t spent = probe.board.runMcu(1u);
		const uint32_t pc1   = probe.pc();

		check(!probe.board.mcuHalted(), "fixture: the field runs without halting the core");
		check(!probe.board.faulted(),   "fixture: the field runs without faulting the core");
		check(pc1 > pc0,                "fixture: a budget of one cycle retired an instruction");
		check(spent > 1u,
			"fixture: a budget of one cycle reported MORE than one cycle, so the cycle debt "
			"can accrue at all and the MCU context can move this Scheduler's state");

		instrCost = static_cast<int64_t>(spent);
	}

	if(instrCost <= 1 || !dsp56k::g_useJIT)
	{
		/* Either the fixture is unusable or no Scheduler can be created at all
		 * (section 11.4.3). Case 1 above needed neither and has already run.
		 * Reporting the reason beats running every later case against a machine
		 * whose failures would all name this one. */
		std::printf("t0_scheduler_state: no Scheduler cases run (cycles/instruction %lld, "
			"g_useJIT %s)\n", static_cast<long long>(instrCost),
			dsp56k::g_useJIT ? "true" : "false");
		std::printf("t0_scheduler_state: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return g_failures == 0 ? 0 : 1;
	}

	/* THE MCU RATE, DERIVED FROM THE MEASUREMENT AND NOT WRITTEN DOWN.
	 *
	 * THE DENOMINATOR IS WHAT MAKES THE ACCUMULATOR MOVE. alloc() walks acc
	 * modulo the denominator, so a rate whose numerator divides its denominator
	 * leaves acc identically zero and the round trip could not tell an
	 * implementation that saved the accumulator from one that did not. The
	 * quantum count below is not a multiple of the denominator either, so the
	 * accumulator's PHASE at the save point differs from its phase a hundred
	 * quanta later -- which is what makes dropping it from the image observable.
	 *
	 * THE NUMERATOR PUTS THE BUDGET BELOW ONE INSTRUCTION'S COST, so the core
	 * overruns, the debt accrues, and the rule 4 long-dispatch branch fires --
	 * both diagnostic counters move rather than one. */
	constexpr uint32_t kRateDen = 7u;
	const ::Rational mcuRate = { static_cast<uint32_t>(4 * instrCost - 1), kRateDen };

	/* THE ROW'S OWN COUNT. */
	constexpr size_t kQuanta = 100u;

	static_assert(kQuanta % kRateDen != 0u,
		"the quantum count must not be a multiple of the rate denominator, or the rational "
		"accumulator holds the same phase at the save point and at the load point and "
		"dropping it from the image is unobservable");

	/* =====================================================================
	 * CASE 2. THE IMAGE IS SIZED BY THE STRUCTURE, AND stateSave WRITES
	 * EXACTLY WHAT stateSize CLAIMS.
	 *
	 * AN IMPLEMENTATION THAT SAVES ZERO BYTES MUST FAIL THIS FILE, which the
	 * row states as its acceptance criterion. This case is the cheapest place
	 * that becomes red, and the bound is DERIVED from the two composed limbs
	 * a caller can reach rather than written down: the Scheduler's block holds
	 * the Board's snapshot, the ChainAdapter's and the DSP set's, plus its own
	 * accumulators, debts, counters, frame index and version word -- so it is
	 * STRICTLY larger than the two limbs a test can size from outside.
	 */
	{
		Rig rig(mcuRate);

		check(rig.scheduler != nullptr, "case 2: the Config yields a Scheduler");
		checkEqualI64(static_cast<int64_t>(rig.status), static_cast<int64_t>(g2::Status::Ok),
			"case 2: the Config is accepted and reports Ok");

		if(rig.scheduler)
		{
			const g2::Scheduler& s = *rig.scheduler;

			const size_t boardBytes = rig.machine.board.stateSize();
			const size_t dspBytes   = rig.machine.board.dspSet().stateSize();

			check(boardBytes > 0u, "case 2: the Board's own snapshot is not empty");
			check(dspBytes > 0u,   "case 2: the DSP set's own snapshot is not empty");

			check(s.stateSize() > boardBytes + dspBytes,
				"case 2: the Scheduler's block is STRICTLY larger than the two composed limbs "
				"a caller can size from outside, so it carries the ChainAdapter's image and "
				"its own accumulators, debts, counters, frame index and version word too -- "
				"a zero-byte snapshot fails here");

			/* stateSize() IS THE FIGURE A CALLER ALLOCATES AGAINST. A save that
			 * wrote past it corrupts the caller's heap; one that stopped short
			 * leaves the tail of the image undefined. Both are caught by a
			 * sentinel-filled buffer with a guard region past the claimed size. */
			const size_t size = s.stateSize();
			constexpr uint8_t kSentinel = 0x5Au;
			constexpr size_t  kGuard    = 64u;

			std::vector<uint8_t> guarded(size + kGuard, kSentinel);
			s.stateSave(guarded.data());

			bool guardIntact = true;
			for(size_t i = size; i < guarded.size(); ++i)
				guardIntact = guardIntact && guarded[i] == kSentinel;

			check(guardIntact,
				"case 2: stateSave writes stateSize() bytes and NOT ONE MORE");

			bool lastByteWritten = false;
			for(size_t i = 0; i < size; ++i)
				lastByteWritten = lastByteWritten || guarded[i] != kSentinel;

			check(lastByteWritten,
				"case 2: stateSave writes into the block stateSize() claims");
		}
	}

	/* =====================================================================
	 * CASE 3. THE ROUND TRIP, IN THE ROW'S OWN SHAPE.
	 *
	 *   run 100 quanta, save, run 100 more, load, run the same 100 again,
	 *   and assert identical output.
	 *
	 * FOUR ASSERTIONS IN ORDER, AND THE ORDER IS LOAD-BEARING:
	 *
	 *   (a) THE STATE MOVED between the save point and the load point. Asserted
	 *       FIRST, because every equality after it is vacuous without it.
	 *   (b) THE LOAD PUT THE MACHINE BACK: the digest and the image immediately
	 *       after the load equal the digest and the image AT THE SAVE POINT.
	 *       A stateLoad that returns Ok without loading leaves both where the
	 *       second hundred quanta left them, and fails here.
	 *   (c) THE REPLAY REPRODUCES: the same hundred quanta run again reach the
	 *       same digest and the same image. This is where a member dropped from
	 *       the image but restored to a plausible value still goes red, because
	 *       the accumulator it dropped changes the budget sequence of every
	 *       quantum after it.
	 *   (d) THE VERSION WORD ROUND-TRIPS, which the image equality at (b) is.
	 */
	{
		Rig rig(mcuRate);

		check(rig.scheduler != nullptr, "case 3: the Config yields a Scheduler");

		if(rig.scheduler)
		{
			g2::Scheduler&  s        = *rig.scheduler;
			const unsigned  dspCount = rig.machine.board.dspSet().dspCount();

			s.runFrames(kQuanta);

			const Digest               digestSave = digestOf(s, dspCount);
			const std::vector<uint8_t> imageSave  = imageOf(s);

			s.runFrames(kQuanta);

			const Digest               digestRun = digestOf(s, dspCount);
			const std::vector<uint8_t> imageRun  = imageOf(s);

			reportDigest(digestSave, "digest at the save point");
			reportDigest(digestRun,  "digest a hundred quanta later");

			/* ---- (a) THE STATE MOVED. */
			check(digestSave.frameIndex == kQuanta,
				"case 3: a hundred quanta advanced the virtual clock by a hundred");
			check(digestRun.frameIndex == 2u * kQuanta,
				"case 3: a hundred more advanced it by a hundred more");

			check(digestRun.ldq[0] > digestSave.ldq[0] && digestSave.ldq[0] > 0u,
				"case 3: the MCU's rule 4 long-dispatch counter ROSE across the second "
				"hundred quanta and was already above zero at the save point -- the state "
				"this round trip compares is not a zero");

			for(unsigned p = 0; p < dspCount; ++p)
			{
				check(digestSave.underrun[p] > 0u,
					"case 3: the chain's underrun counter is above zero at the save point, so "
					"the ChainAdapter limb of the image is not a zero either" + atIndex(p));
				check(digestRun.underrun[p] > digestSave.underrun[p],
					"case 3: it ROSE across the second hundred quanta" + atIndex(p));
			}

			check(!(digestSave == digestRun),
				"case 3: the digest MOVED between the save point and the load point");
			checkImagesDiffer(imageSave, imageRun,
				"case 3: the IMAGE moved between the save point and the load point");

			check(!digestRun.faulted,
				"case 3: two hundred quanta over the field faulted no context, so no "
				"equality below is an equality between two halted machines");

			/* ---- (b) THE LOAD PUT THE MACHINE BACK. */
			checkEqualI64(static_cast<int64_t>(s.stateLoad(imageSave.data())),
				static_cast<int64_t>(g2::Status::Ok),
				"case 3: stateLoad takes back the image this Scheduler wrote -- which it can "
				"only do because it detaches the Board's HDI08 bridges around the DSP limb");

			const Digest               digestAfterLoad = digestOf(s, dspCount);
			const std::vector<uint8_t> imageAfterLoad  = imageOf(s);

			reportDigest(digestAfterLoad, "digest immediately after the load");

			check(digestAfterLoad == digestSave,
				"case 3: the digest immediately after the load equals the digest AT THE SAVE "
				"POINT -- a stateLoad that returned Ok without loading leaves it where the "
				"second hundred quanta left it");
			checkImagesEqual(imageAfterLoad, imageSave,
				"case 3: the image immediately after the load equals the image at the save "
				"point, byte for byte, VERSION WORD INCLUDED -- this is where the rational "
				"accumulators are compared, at every context index, since the class exposes "
				"them through no accessor");

			for(unsigned i = 0; i <= dspCount; ++i)
			{
				checkEqualI64(digestAfterLoad.debt[i], digestSave.debt[i],
					"case 3: cycleDebt is restored, index 0 included" + atIndex(i));
				checkEqualI64(static_cast<int64_t>(digestAfterLoad.ldq[i]),
					static_cast<int64_t>(digestSave.ldq[i]),
					"case 3: longDispatchQuanta is restored, index 0 included" + atIndex(i));
			}

			/* ---- (c) THE REPLAY REPRODUCES. */
			s.runFrames(kQuanta);

			const Digest               digestReplay = digestOf(s, dspCount);
			const std::vector<uint8_t> imageReplay  = imageOf(s);

			reportDigest(digestReplay, "digest after the replay");

			check(digestReplay == digestRun,
				"case 3: the same hundred quanta run again reach the SAME digest -- this is "
				"the row's 'identical output'");
			checkImagesEqual(imageReplay, imageRun,
				"case 3: and the same image, byte for byte");
		}
	}

	/* =====================================================================
	 * CASE 4. A PERTURBED VERSION WORD IS A NAMED LOAD FAILURE.
	 *
	 * The row: "Assert the version word round-trips and that a perturbed
	 * version word is a named load failure rather than a silent acceptance."
	 * The round trip is case 3's image equality; the refusal is here.
	 *
	 * THE REFUSAL CHANGES NOTHING, and that half matters as much as the status:
	 * a load that reported BadStateImage after writing half the block would
	 * leave a machine no run produced. The version word is the FIRST field of
	 * the block for exactly this reason, so the comparison happens before the
	 * first write.
	 */
	{
		Rig rig(mcuRate);

		check(rig.scheduler != nullptr, "case 4: the Config yields a Scheduler");

		if(rig.scheduler)
		{
			g2::Scheduler& s        = *rig.scheduler;
			const unsigned dspCount = rig.machine.board.dspSet().dspCount();

			s.runFrames(kQuanta);

			std::vector<uint8_t> image = imageOf(s);
			check(image.size() > sizeof(uint32_t),
				"case 4: the image is larger than the version word it leads with");

			s.runFrames(kQuanta);

			const Digest               before      = digestOf(s, dspCount);
			const std::vector<uint8_t> imageBefore = imageOf(s);

			/* THE PERTURBATION IS A BIT FLIP AND NOT A CHOSEN VALUE. Writing a
			 * number here would pin the version this build happens to carry,
			 * and the next revision of the block would have to remember to
			 * change it. Any flip of the leading word is a version this build
			 * does not write. */
			for(size_t i = 0; i < sizeof(uint32_t); ++i)
				image[i] = static_cast<uint8_t>(image[i] ^ 0xFFu);

			checkEqualI64(static_cast<int64_t>(s.stateLoad(image.data())),
				static_cast<int64_t>(g2::Status::BadStateImage),
				"case 4: an image whose version word this build does not write is refused by "
				"NAME rather than silently accepted");

			check(digestOf(s, dspCount) == before,
				"case 4: the refused load changed nothing this Scheduler reports");
			checkImagesEqual(imageOf(s), imageBefore,
				"case 4: the refused load changed nothing this Scheduler saves either -- the "
				"version word is compared BEFORE the first write");
		}
	}

	/* =====================================================================
	 * CASE 5. THE DSP LIMB, DRIVEN BY HAND THROUGH THE SCHEDULER.
	 *
	 * THIS IS THE CASE THE MIRAGE MAKES NECESSARY. No firmware lands in a T0
	 * fixture, every run gate is shut, and DSP memory never changes by itself,
	 * so the DSP half of case 3's image equality is an equality between two
	 * blocks of zeros. It would pass against a composition that dropped the DSP
	 * limb entirely -- which is the half section 24.6 row W3-415 says a previous
	 * pass refused to ship.
	 *
	 * SO THE STATE IS MOVED BY HAND: a distinct generation is written into every
	 * slot's register block and into P, X and Y at both ends of every area, the
	 * SCHEDULER saves, a second generation is written, the SCHEDULER loads, and
	 * the first generation is read back. Every word compared here is non-zero
	 * and every word differs between the two generations.
	 *
	 * IT ALSO PROVES THE BRACKET. The set is the BOARD'S set and it holds its
	 * bridges throughout, so a Scheduler::stateLoad that did not detach them
	 * would answer BridgesAttached and restore nothing -- and a re-attach that
	 * did not put them back would leave the set unable to refuse afterwards.
	 * Both are asserted.
	 */
	{
		Rig rig(mcuRate);

		check(rig.scheduler != nullptr, "case 5: the Config yields a Scheduler");

		if(rig.scheduler)
		{
			g2::Scheduler& s   = *rig.scheduler;
			g2::DspSet&    set = rig.machine.board.dspSet();

			check(set.bridgesAttached(),
				"case 5: the Scheduler's DSP set holds its bridges, which is the condition "
				"DspSet::stateLoad refuses on");

			/* A DISTINCT, NON-ZERO WORD FOR EVERY (generation, slot, cell). */
			const auto word = [](const unsigned _gen, const unsigned _slot, const unsigned _cell)
			{
				return static_cast<dsp56k::TWord>(
					(0x100000u + (_gen << 16) + (_slot << 8) + _cell) & 0x7FFFFFu);
			};

			const dsp56k::EMemArea areas[] =
				{ dsp56k::MemArea_P, dsp56k::MemArea_X, dsp56k::MemArea_Y };

			/* TWO CELLS PER AREA, BOTH BELOW THE BRIDGED REGION. dspSet.cpp
			 * builds every slot's memory with a bridged X/Y address, above
			 * which a write to X is a write to Y -- so a probe at the TOP of
			 * each area would read another area's word back and this file would
			 * report a defect of its own fixture. Both offsets are inside P's
			 * smaller size as well as inside X's and Y's. */
			constexpr dsp56k::TWord kLowCell  = 0x001000u;
			constexpr dsp56k::TWord kHighCell = 0x01FFFFu;

			const auto apply = [&](const unsigned _gen)
			{
				for(unsigned i = 0; i < set.dspCount(); ++i)
				{
					dsp56k::DSP::SRegs& regs = set.dsp(i).regs();
					regs.r[0].var = static_cast<int32_t>(word(_gen, i, 1u));
					regs.n[0].var = static_cast<int32_t>(word(_gen, i, 2u));

					dsp56k::Memory& mem = set.dsp(i).memory();

					for(unsigned a = 0; a < 3u; ++a)
					{
						mem.getMemAreaPtr(areas[a])[kLowCell]  = word(_gen, i, 0x10u + a);
						mem.getMemAreaPtr(areas[a])[kHighCell] = word(_gen, i, 0x20u + a);
					}
				}
			};

			const auto expect = [&](const unsigned _gen, const std::string& _what)
			{
				for(unsigned i = 0; i < set.dspCount(); ++i)
				{
					const dsp56k::DSP::SRegs& regs = set.dsp(i).regs();
					checkEqualI64(regs.r[0].var, static_cast<int64_t>(word(_gen, i, 1u)),
						"case 5: r0 " + _what + atIndex(i));
					checkEqualI64(regs.n[0].var, static_cast<int64_t>(word(_gen, i, 2u)),
						"case 5: n0 " + _what + atIndex(i));

					dsp56k::Memory& mem = set.dsp(i).memory();

					for(unsigned a = 0; a < 3u; ++a)
					{
						checkEqualI64(mem.getMemAreaPtr(areas[a])[kLowCell],
							static_cast<int64_t>(word(_gen, i, 0x10u + a)),
							"case 5: low cell of area " + std::to_string(a) + " " + _what + atIndex(i));
						checkEqualI64(mem.getMemAreaPtr(areas[a])[kHighCell],
							static_cast<int64_t>(word(_gen, i, 0x20u + a)),
							"case 5: high cell of area " + std::to_string(a) + " " + _what + atIndex(i));
					}
				}
			};

			apply(1u);

			const std::vector<uint8_t> image = imageOf(s);

			apply(2u);
			expect(2u, "carries the second generation before the load");

			checkEqualI64(static_cast<int64_t>(s.stateLoad(image.data())),
				static_cast<int64_t>(g2::Status::Ok),
				"case 5: the Scheduler takes the image back over a BRIDGED DSP set, which it "
				"can only do by detaching the bridges around the DSP limb");

			expect(1u, "is restored by the round trip");

			check(set.bridgesAttached(),
				"case 5: the set is wearing its bridges again once the load has returned");

			std::vector<uint8_t> dspImage(set.stateSize());
			set.stateSave(dspImage.data());

			checkEqualI64(static_cast<int64_t>(set.stateLoad(dspImage.data())),
				static_cast<int64_t>(g2::Status::BridgesAttached),
				"case 5: and it refuses a direct load again, so the re-attach restored the "
				"REFUSAL and not merely a container");
		}
	}

	std::printf("t0_scheduler_state: %d failure(s) in %d case(s)\n", g_failures, g_cases);
	return g_failures == 0 ? 0 : 1;
}
