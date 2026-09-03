/* t0_state_excludes_regime.cpp -- the check of SCH-36. Design sections 15.6
 * (the boot sequence's regime rule) and 13.10 rule 3.
 *
 * THE DEFECT THIS FILE PINS, AS PLG-12 MEASURED IT. Scheduler::stateSave wrote
 * the CODEC REGIME into its own limb of the state block, so stateLoad restored
 * it. A snapshot is necessarily a PLAY-regime snapshot -- a host takes one
 * through getState, which runs after the boot published the machine -- so
 * design section 15.6's step 3 put step 4 into the PLAY regime. Step 4's boot
 * quanta then touched the codec queues, the sink filled after L + B pushes, the
 * run stopped part-way through the boot and beginPlayPhase's debug assert
 * aborted. PLG-12 could not repair it, because scheduler.{h,cpp} is not on its
 * Files: line.
 *
 * THE REPAIR IS EXCLUSION AT stateSave AND THIS FILE ASSERTS THE BEHAVIOUR AND
 * NOT THE FIELD. Nothing here reads a byte offset and nothing here counts the
 * block's size. Every regime claim is made through what the machine DOES with a
 * quantum, which is the only thing the class exposes: scheduler.h keeps
 * CodecRegime private and this file adds no accessor.
 *
 * WHAT "THE REGIME THE MACHINE REPORTS" IS, STATED ONCE. A BOOT quantum emits
 * the five unconditional phases -- swap, panel, sof, mcu, dsp -- and a PLAY
 * quantum emits seven, with the ingress at index 1 and the egress at index 6.
 * t0_codec_regimes establishes that separation; this file uses it as the
 * instrument. THE INSTRUMENT CARRIES ITS OWN KNOWN POSITIVE: case 0 observes a
 * seven-record quantum on the DONOR before the snapshot is taken, so a run in
 * which every quantum looked like a boot quantum -- a broken trace, a regime
 * that never moved -- cannot pass case 1 by accident.
 *
 * NOTHING HERE IS A LANGUAGE assert() AND NOTHING CATCHES AN EXCEPTION. Every
 * verdict is the failure counter, which no build type removes. The default
 * build is Release and Release defines NDEBUG.
 *
 * NO COUNT IS TYPED. The queue capacities come from the two Config fields, the
 * DSP count from the Board's set, the boot-quantum count from the capacity, and
 * the MCU rate from a measurement of the linked core.
 */

#include "board.h"
#include "dspSet.h"
#include "executor.h"
#include "frame.h"
#include "memoryMap.h"
#include "scheduler.h"
#include "status.h"

#include "dsp56kEmu/dsp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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

	void checkEqual(const uint64_t _observed, const uint64_t _expected, const std::string& _what)
	{
		++g_cases;

		if(_observed == _expected)
			return;

		std::printf("FAIL %s: observed %llu, expected %llu\n", _what.c_str(),
			static_cast<unsigned long long>(_observed), static_cast<unsigned long long>(_expected));
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

	const char* phaseName(const g2::TracePhase _phase)
	{
		switch(_phase)
		{
		case g2::TracePhase::Swap:    return "swap";
		case g2::TracePhase::Ingress: return "ingress";
		case g2::TracePhase::Panel:   return "panel";
		case g2::TracePhase::Sof:     return "sof";
		case g2::TracePhase::Mcu:     return "mcu";
		case g2::TracePhase::Dsp:     return "dsp";
		case g2::TracePhase::Egress:  return "egress";
		}
		return "?";
	}

	class RecordingTrace final : public g2::TraceSink
	{
	public:
		static constexpr size_t kMax = 4096;

		void onPhase(const g2::TracePhase _phase, const uint64_t _frameIndex) noexcept override
		{
			if(m_count < kMax)
			{
				m_phase[m_count] = _phase;
				m_frame[m_count] = _frameIndex;
			}
			++m_count;
		}

		size_t         count()               const { return m_count; }
		g2::TracePhase phase(const size_t i) const { return m_phase[i]; }
		void           clear()                     { m_count = 0; }

	private:
		size_t         m_count = 0;
		g2::TracePhase m_phase[kMax]{};
		uint64_t       m_frame[kMax]{};
	};

	/* THE TWO QUANTA, COPIED FROM DESIGN SECTION 13.5's ORDER. t0_codec_regimes
	 * owns the ordering claim; this file only needs to tell one regime from the
	 * other. */
	constexpr g2::TracePhase kBootQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp
	};

	constexpr g2::TracePhase kPlayQuantum[] =
	{
		g2::TracePhase::Swap,
		g2::TracePhase::Ingress,
		g2::TracePhase::Panel,
		g2::TracePhase::Sof,
		g2::TracePhase::Mcu,
		g2::TracePhase::Dsp,
		g2::TracePhase::Egress
	};

	constexpr size_t kBootPhases = sizeof(kBootQuantum) / sizeof(kBootQuantum[0]);
	constexpr size_t kPlayPhases = sizeof(kPlayQuantum) / sizeof(kPlayQuantum[0]);

	constexpr unsigned kLookahead    = 4;   /* L */
	constexpr unsigned kMaxHostBlock = 3;   /* B */

	/* BOTH QUEUE CAPACITIES ARE L + B, design section 13.6.1, and a boot run of
	 * TWICE that is more quanta than either queue could hold -- which is what
	 * makes "the run did not stall" a claim with teeth rather than a run too
	 * short to fill anything. Neither figure is a literal. */
	constexpr size_t   kCapacity   = static_cast<size_t>(kLookahead) + kMaxHostBlock;
	constexpr unsigned kBootQuanta = static_cast<unsigned>(2 * kCapacity);

	/* THE REGIME A MACHINE REPORTS, READ OFF ONE QUANTUM. The verdict is the
	 * PHASE SEQUENCE and not the record count alone: a count would be satisfied
	 * by five records in the wrong order. */
	enum class ObservedRegime { Boot, Play, Neither };

	ObservedRegime observeRegime(g2::Scheduler& _s, RecordingTrace& _trace)
	{
		_trace.clear();
		_s.runFrames(1);

		if(_trace.count() == kBootPhases)
		{
			for(size_t i = 0; i < kBootPhases; ++i)
			{
				if(_trace.phase(i) != kBootQuantum[i])
					return ObservedRegime::Neither;
			}
			return ObservedRegime::Boot;
		}

		if(_trace.count() == kPlayPhases)
		{
			for(size_t i = 0; i < kPlayPhases; ++i)
			{
				if(_trace.phase(i) != kPlayQuantum[i])
					return ObservedRegime::Neither;
			}
			return ObservedRegime::Play;
		}

		return ObservedRegime::Neither;
	}

	std::string spell(const ObservedRegime _r)
	{
		switch(_r)
		{
		case ObservedRegime::Boot: return "BOOT (five phases, no ingress and no egress)";
		case ObservedRegime::Play: return "PLAY (seven phases, ingress at 1 and egress at 6)";
		default:                   return "NEITHER (the quantum matched no declared regime)";
		}
	}

	std::string spellTrace(const RecordingTrace& _trace)
	{
		std::string s;

		for(size_t i = 0; i < _trace.count() && i < 16; ++i)
		{
			s += i == 0 ? "" : " ";
			s += phaseName(_trace.phase(i));
		}

		return s.empty() ? "(no phase records)" : s;
	}

	/* ---------------------------------------------------------------------
	 * THE FIXTURE. A Board with an SDRAM window full of one repeated
	 * instruction and a core reset into it -- the machine t0_mcu_debt and
	 * t0_scheduler_state drive, for the same reason: it is the ONE part of a T0
	 * Scheduler whose emulated state moves, and case 2's round trip over a
	 * machine whose state never moves would be an equality of zeros.
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

	/* NOP. */
	constexpr uint16_t g_nop = 0x4E71u;

	constexpr uint32_t g_windowBase = g2::g_sdramBase;
	constexpr uint32_t g_windowSize = 0x4000u;
	constexpr uint32_t g_stackTop   = g_windowBase + g_windowSize;
	constexpr uint32_t g_codeBase   = g_windowBase;

	g2::BoardConfig makeBoardConfig()
	{
		g2::BoardConfig config;
		config.memory.sdram = { g_windowBase, g_windowSize };
		return config;
	}

	struct Machine
	{
		Ram       ram{ g_windowSize };
		g2::Board board{ makeBoardConfig() };

		Machine()
		{
			for(uint32_t offset = 0; offset + 1u < g_windowSize; offset += 2u)
				ram.pokeWord(offset, g_nop);

			board.memory().attach(g2::Region::Sdram, &ram);
			board.resetMcu(g_stackTop, g_codeBase);
		}

		uint32_t pc() const { return board.mcuReg(17); }
	};

	/* ONE MACHINE AND ONE Scheduler OVER IT. THE DECLARATION ORDER IS THE
	 * LIFETIME: the Board must outlive the Scheduler, and a member declared
	 * later is destroyed first. */
	struct Rig
	{
		Machine                        machine;
		g2::SerialExecutor             executor;
		RecordingTrace                 trace;
		g2::Status                     status{};
		std::unique_ptr<g2::Scheduler> scheduler;

		explicit Rig(const ::Rational _mcuRate)
		{
			g2::Scheduler::Config config;
			config.mcuRate            = _mcuRate;
			config.lookaheadFrames    = kLookahead;
			config.maxHostBlockFrames = kMaxHostBlock;
			config.trace              = &trace;

			scheduler = g2::Scheduler::create(config, executor, machine.board, status);
		}
	};

	/* EVERY STATE ITEM SCH-21's BLOCK CARRIES THAT A CALLER CAN READ, one field
	 * for one item. Case 2 compares this STRUCT ITEM BY ITEM and names the item
	 * that moved, which is what makes an over-broad exclusion -- a truncation --
	 * red BY THE LOST ITEM rather than red in general. */
	struct Digest
	{
		uint64_t              frameIndex = 0;
		std::vector<uint64_t> underrun;
		std::vector<uint64_t> secondUnderrun;
		std::vector<uint64_t> phaseError;
		std::vector<int64_t>  debt;
		std::vector<uint64_t> ldq;
		std::vector<uint32_t> fault;
		uint8_t               faulted    = 0;
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

		/* INDEX 0 IS THE MCU AND 1 .. dspCount ARE THE DSPs. */
		for(unsigned i = 0; i <= _dspCount; ++i)
		{
			d.debt.push_back(_s.cycleDebt(i));
			d.ldq.push_back(_s.longDispatchQuanta(i));
			d.fault.push_back(static_cast<uint32_t>(_s.contextFault(i)));
		}

		d.faulted = _s.faulted() ? 1u : 0u;

		return d;
	}

	bool sameDigest(const Digest& _a, const Digest& _b)
	{
		return _a.frameIndex     == _b.frameIndex
			&& _a.underrun       == _b.underrun
			&& _a.secondUnderrun == _b.secondUnderrun
			&& _a.phaseError     == _b.phaseError
			&& _a.debt           == _b.debt
			&& _a.ldq            == _b.ldq
			&& _a.fault          == _b.fault
			&& _a.faulted        == _b.faulted;
	}

	/* THE ITEM-BY-ITEM COMPARISON. Every message names the ITEM, so a
	 * truncation that drops one of them reports WHICH one. */
	void checkDigestsEqual(const Digest& _observed, const Digest& _expected, const std::string& _tag)
	{
		checkEqual(_observed.frameIndex, _expected.frameIndex,
			_tag + ": item 'the virtual frame index' survives the round trip by value");

		checkEqual(_observed.underrun.size(), _expected.underrun.size(),
			_tag + ": the counter baselines cover the same positions");

		const size_t positions = _observed.underrun.size() < _expected.underrun.size()
			? _observed.underrun.size() : _expected.underrun.size();

		for(size_t p = 0; p < positions; ++p)
		{
			const unsigned i = static_cast<unsigned>(p);

			checkEqual(_observed.underrun[p], _expected.underrun[p],
				_tag + ": item 'the audio-bus underrun baseline' survives by value" + atIndex(i));
			checkEqual(_observed.secondUnderrun[p], _expected.secondUnderrun[p],
				_tag + ": item 'the second-bus underrun baseline' survives by value" + atIndex(i));
			checkEqual(_observed.phaseError[p], _expected.phaseError[p],
				_tag + ": item 'the phase-error baseline' survives by value" + atIndex(i));
		}

		checkEqual(_observed.debt.size(), _expected.debt.size(),
			_tag + ": the context items cover the same contexts");

		const size_t contexts = _observed.debt.size() < _expected.debt.size()
			? _observed.debt.size() : _expected.debt.size();

		for(size_t c = 0; c < contexts; ++c)
		{
			const unsigned i = static_cast<unsigned>(c);

			checkEqualI64(_observed.debt[c], _expected.debt[c],
				_tag + ": item 'the cycle debt' survives by value" + atIndex(i));
			checkEqual(_observed.ldq[c], _expected.ldq[c],
				_tag + ": item 'the rule 4 long-dispatch counter' survives by value" + atIndex(i));
			checkEqual(_observed.fault[c], _expected.fault[c],
				_tag + ": item 'the sticky fault latch' survives by value" + atIndex(i));
		}

		checkEqual(_observed.faulted, _expected.faulted,
			_tag + ": item 'the fault disjunction' survives the round trip by value");
	}

	std::vector<uint8_t> imageOf(const g2::Scheduler& _s)
	{
		std::vector<uint8_t> image(_s.stateSize(), 0u);

		if(!image.empty())
			_s.stateSave(image.data());

		return image;
	}

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
}

int main()
{
	std::printf("t0_state_excludes_regime: g_useJIT = %s\n", dsp56k::g_useJIT ? "true" : "false");

	/* THE MCU RATE IS DERIVED FROM A MEASUREMENT OF THE LINKED CORE AND NOT
	 * WRITTEN DOWN, on t0_scheduler_state's form and for its reason: the
	 * denominator is what makes the rational accumulator move and the numerator
	 * is what makes the cycle debt accrue, so case 2's items are items that
	 * actually CHANGE between the save point and the load point. */
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
			"fixture: a budget of one cycle reported MORE than one cycle, so the cycle debt can "
			"accrue at all and this Scheduler's state can move");

		instrCost = static_cast<int64_t>(spent);
	}

	if(instrCost <= 1 || !dsp56k::g_useJIT)
	{
		std::printf("t0_state_excludes_regime: no Scheduler cases run (cycles/instruction %lld, "
			"g_useJIT %s)\n", static_cast<long long>(instrCost),
			dsp56k::g_useJIT ? "true" : "false");
		std::printf("t0_state_excludes_regime: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return g_failures == 0 ? 0 : 1;
	}

	constexpr uint32_t kRateDen = 7u;
	const ::Rational mcuRate = { static_cast<uint32_t>(4 * instrCost - 1), kRateDen };

	constexpr size_t kQuanta = 100u;

	static_assert(kQuanta % kRateDen != 0u,
		"the quantum count must not be a multiple of the rate denominator, or the rational "
		"accumulator holds the same phase at the save point and at the load point");

	/* =====================================================================
	 * CASE 0. THE DONOR, AND THE KNOWN POSITIVE FOR THE INSTRUMENT.
	 *
	 * The donor runs its boot quanta, hands off through beginPlayPhase, and is
	 * then OBSERVED to be in the PLAY regime by the same instrument every later
	 * case reads. Without this the whole file could pass against a machine that
	 * never leaves the boot regime and against a trace that emits nothing.
	 *
	 * ITS SNAPSHOT IS THEREFORE A PLAY-REGIME SNAPSHOT -- which is exactly what
	 * a host's getState takes, and the input the defect needed.
	 */
	Rig donor(mcuRate);

	check(donor.scheduler != nullptr, "case 0: the donor Config yields a Scheduler");
	checkEqualI64(static_cast<int64_t>(donor.status), static_cast<int64_t>(g2::Status::Ok),
		"case 0: the donor Config is accepted and reports Ok");

	if(!donor.scheduler)
	{
		std::printf("t0_state_excludes_regime: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	const unsigned dspCount = donor.machine.board.dspSet().dspCount();

	check(dspCount > 0u, "case 0: the Board's DSP set holds at least one slot");

	std::vector<uint8_t> playImage;

	{
		g2::Scheduler& d = *donor.scheduler;

		d.runFrames(kBootQuanta);

		checkEqual(d.frameIndex(), kBootQuanta,
			"case 0: the donor's own boot ran every quantum it was asked for");

		d.beginPlayPhase();

		const ObservedRegime observed = observeRegime(d, donor.trace);

		check(observed == ObservedRegime::Play,
			"case 0 KNOWN POSITIVE: after beginPlayPhase the donor reports the PLAY regime, so "
			"the snapshot taken below is a play-regime snapshot and the instrument can tell the "
			"two regimes apart -- observed " + spell(observed) +
			", phases: " + spellTrace(donor.trace));

		playImage = imageOf(d);

		check(!playImage.empty(), "case 0: the donor's snapshot is not empty");
	}

	/* =====================================================================
	 * CASE 1. A PLAY-REGIME SNAPSHOT LOADED INTO A BOOT-REGIME MACHINE LEAVES
	 * THE BOOT REGIME STANDING.
	 *
	 * THE VERDICT IS THE REGIME THE MACHINE REPORTS. Not a byte offset, not the
	 * block's size, not a field. REQUIRED-RED: restore the regime write in
	 * stateSave (or the read in stateLoad) and this case goes red naming the
	 * PLAY regime it observed where the BOOT regime was required.
	 */
	{
		Rig recipient(mcuRate);

		check(recipient.scheduler != nullptr, "case 1: the recipient Config yields a Scheduler");

		if(recipient.scheduler)
		{
			g2::Scheduler& r = *recipient.scheduler;

			/* A FRESH Scheduler IS BORN IN THE BOOT REGIME, and that is asserted
			 * rather than assumed: the case below claims the load LEFT it
			 * standing, which says nothing unless it was standing first. */
			const ObservedRegime before = observeRegime(r, recipient.trace);

			check(before == ObservedRegime::Boot,
				"case 1: a freshly created Scheduler reports the BOOT regime before the load -- "
				"observed " + spell(before));

			const g2::Status status = r.stateLoad(playImage.data());

			checkEqualI64(static_cast<int64_t>(status), static_cast<int64_t>(g2::Status::Ok),
				"case 1: the play-regime image is accepted by a boot-regime machine");

			const ObservedRegime after = observeRegime(r, recipient.trace);

			check(after == ObservedRegime::Boot,
				"case 1: THE REGIME DOES NOT TRAVEL. A play-regime state block loaded into a "
				"boot-regime machine leaves the BOOT regime standing -- observed " + spell(after) +
				", phases: " + spellTrace(recipient.trace));

			/* THE SAME CLAIM FROM THE OTHER SIDE, and it is the consequence
			 * design section 15.6 cares about: a boot-regime run cannot stall
			 * on a full sink. 2 x (L + B) quanta is more than either queue
			 * holds, so a play regime here would stop after B + 1. */
			const uint64_t before2 = r.frameIndex();

			r.runFrames(kBootQuanta);

			checkEqual(r.frameIndex() - before2, kBootQuanta,
				"case 1: every quantum after the load ran -- the boot regime never touches the "
				"sink, so the sink cannot fill and the run cannot stop part-way");
		}
	}

	/* =====================================================================
	 * CASE 2. THE EXCLUSION IS SURGICAL AND NOT A TRUNCATION.
	 *
	 * Every OTHER item SCH-21's block carries is recovered BY VALUE. The
	 * comparison is item by item and every message names its item, so the
	 * second REQUIRED-RED -- drop one unrelated item along with the regime --
	 * goes red NAMING THE LOST ITEM.
	 *
	 * THE NON-VACUITY GUARD RUNS FIRST. The digest at the save point and the
	 * digest at the load point must DIFFER, and the two images must differ
	 * byte-wise; without that, an implementation that saved nothing at all would
	 * satisfy every equality below.
	 *
	 * THE PERTURBATION BETWEEN THE SAVE AND THE LOAD IS A HUNDRED QUANTA AND A
	 * beginPlayPhase, AND THE SECOND HALF IS LOAD-BEARING RATHER THAN DECORATIVE.
	 * MEASURED: with a hundred quanta alone, dropping the three adapter-owned
	 * counter baselines from the block is INVISIBLE -- nothing but
	 * beginPlayPhase and reset ever writes them, so a same-machine round trip
	 * restores a value that never moved and an equality of two identical numbers
	 * cannot report the loss. beginPlayPhase re-baselines all three, so the
	 * baselines at the load point differ from the baselines at the save point and
	 * a block that stopped carrying them is red. This is exactly the truncation
	 * the second REQUIRED-RED plants.
	 */
	{
		Rig rig(mcuRate);

		check(rig.scheduler != nullptr, "case 2: the Config yields a Scheduler");

		if(rig.scheduler)
		{
			g2::Scheduler& s = *rig.scheduler;

			s.runFrames(kQuanta);

			const Digest               atSave = digestOf(s, dspCount);
			const std::vector<uint8_t> image  = imageOf(s);

			s.runFrames(kQuanta);
			s.beginPlayPhase();

			const Digest               later      = digestOf(s, dspCount);
			const std::vector<uint8_t> laterImage = imageOf(s);

			/* THE BASELINES REALLY MOVED, asserted rather than assumed: the
			 * equality this case makes about them says nothing if they hold the
			 * same value at both ends. */
			for(unsigned p = 0; p < dspCount; ++p)
			{
				check(later.underrun[p] < atSave.underrun[p],
					"case 2 NON-VACUITY: the audio-bus underrun counter reads SMALLER after "
					"beginPlayPhase than it did at the save point (" +
					std::to_string(later.underrun[p]) + " against " +
					std::to_string(atSave.underrun[p]) + ") -- the ChainAdapter's own reading "
					"only grows, so a smaller reported value can only mean the BASELINE moved, "
					"and an equality over it at the load point can therefore report a block "
					"that stopped carrying it" + atIndex(p));
			}

			check(!sameDigest(atSave, later),
				"case 2 NON-VACUITY: a hundred more quanta moved the observable state, so the "
				"equalities below are not an equality of zeros");

			check(image.size() == laterImage.size() && image != laterImage,
				"case 2 NON-VACUITY: the two images differ byte-wise (first difference at byte " +
				std::to_string(firstDifference(image, laterImage)) + " of " +
				std::to_string(image.size()) + ")");

			const g2::Status status = s.stateLoad(image.data());

			checkEqualI64(static_cast<int64_t>(status), static_cast<int64_t>(g2::Status::Ok),
				"case 2: the image is accepted");

			checkDigestsEqual(digestOf(s, dspCount), atSave, "case 2");

			/* THE ACCUMULATORS TRAVEL THROUGH NO ACCESSOR, so the image itself
			 * is the only statement that can be made about them. A re-save that
			 * reproduces the saved image byte for byte covers every item the
			 * struct above cannot reach. */
			const std::vector<uint8_t> reSaved = imageOf(s);

			check(reSaved.size() == image.size() && reSaved == image,
				"case 2: item 'the rational accumulators, which no accessor exposes' survives "
				"too -- the re-saved image equals the saved image byte for byte (sizes " +
				std::to_string(reSaved.size()) + " and " + std::to_string(image.size()) +
				", first difference at byte " + std::to_string(firstDifference(reSaved, image)) + ")");
		}
	}

	/* =====================================================================
	 * CASE 3. DESIGN SECTION 15.6, STEP 3 AND STEP 4, AND IT IS THE PROPERTY
	 * THE DEFECT BROKE.
	 *
	 * The boot's step 2 resets, step 3 loads the host's snapshot, and step 4
	 * runs its boot quanta. AT THE END OF STEP 4 NEITHER CODEC QUEUE HAS BEEN
	 * TOUCHED -- asserted through the four counters PLG-12's t1_boot_on_restore
	 * reads, and then through the two queue depths themselves.
	 *
	 * THE FOUR COUNTERS ARE READ BEFORE THE TWO DEPTH PROBES, because both
	 * probes mutate.
	 */
	{
		Rig rig(mcuRate);

		check(rig.scheduler != nullptr, "case 3: the Config yields a Scheduler");

		if(rig.scheduler)
		{
			g2::Scheduler& s = *rig.scheduler;

			// STEP 2. THE ENTRY-POINT SETUP FOLLOWS THE RESET AND DOES NOT
			// PRECEDE IT: Scheduler::reset resets the Board's core, so a
			// resetMcu written before it is erased. That is section 24.6 row
			// W3-462's first PLG-12 finding, and this fixture obeys it.
			s.reset();
			rig.machine.board.resetMcu(g_stackTop, g_codeBase);

			// STEP 3.
			const g2::Status status = s.stateLoad(playImage.data());

			checkEqualI64(static_cast<int64_t>(status), static_cast<int64_t>(g2::Status::Ok),
				"case 3: step 3 loaded the host's play-regime snapshot");

			// STEP 4.
			const uint64_t before = s.frameIndex();

			s.runFrames(kBootQuanta);

			checkEqual(s.frameIndex() - before, kBootQuanta,
				"case 3: step 4 ran every one of its quanta -- the run did not stop part-way, "
				"which is what a filled sink would have done");

			checkEqual(s.starvedFrames(),   0u, "case 3: step 4 consumed no source frame");
			checkEqual(s.overflowFrames(),  0u, "case 3: step 4 pushed nothing at the source");
			checkEqual(s.droppedFrames(),   0u, "case 3: step 4 pushed no sink frame");
			checkEqual(s.underflowFrames(), 0u, "case 3: step 4 took nothing from the sink");

			/* THE TWO DEPTHS, MEASURED LAST BECAUSE BOTH PROBES MUTATE. An
			 * untouched sink is EMPTY and an untouched source is EMPTY, so the
			 * sink supplies nothing and the source accepts its whole capacity.
			 * A counter that was never wired would pass the four above and fail
			 * these two. */
			{
				std::vector<g2::Frame> out(kCapacity + 1);

				const size_t pulled = s.pull(out.data(), kCapacity + 1);

				checkEqual(pulled, 0u,
					"case 3: the CodecSink is EMPTY at the end of step 4 -- step 4 never pushed "
					"into it");
			}

			{
				const std::vector<g2::Frame> in(kCapacity + 1);

				const size_t pushed = s.push(in.data(), kCapacity + 1);

				checkEqual(pushed, kCapacity,
					"case 3: the CodecSource is EMPTY at the end of step 4 -- it accepts its "
					"whole capacity, so step 4 consumed nothing from it and left nothing in it");
			}
		}
	}

	if(g_failures != 0)
	{
		std::printf("t0_state_excludes_regime: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_state_excludes_regime: all %d cases passed\n", g_cases);
	return 0;
}
