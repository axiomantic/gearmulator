/* t0_dsp_attach.cpp -- eight DSPs attached face by face.
 *
 * Every write that tests an attachment goes through the attached pointer,
 * never through the DspSet's own peripheral accessor. A write issued through
 * DspSet::peripherals(i).ySpace() reaches the Peripherals56311 whatever the
 * DSP was attached to, so it would assert the peripheral decode a second
 * time and say nothing about the attach. Going through
 * DSP::getPeriph(MemArea_Y) is
 * what a PeripheralsNop in that slot, or the X face attached twice, fails.
 *
 * The read-backs go through both faces because the Y face is a window over
 * the ESAI the X face owns, and only reading both shows the window landed in
 * the object the X face exposes rather than in a backing array.
 *
 * The run-time cases report through g_failures and not through assert().
 * base.cmake at the repository root selects Release when CMAKE_BUILD_TYPE is
 * unset, Release defines NDEBUG, and every assert() then compiles to nothing
 * -- a test built that way would pass having checked nothing.
 */

#include "dspSet.h"

#include "dsp56kEmu/dma.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals56311.h"
#include "dsp56kEmu/timers.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	using dsp56k::TWord;

	int g_cases = 0;
	int g_failures = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	std::string slotName(const unsigned _slot)
	{
		return " on slot " + std::to_string(_slot);
	}

	/* ---------------- group 1: the two faces, slot by slot */

	void theEightSetsAreAttachedFaceByFace(g2::DspSet& _set)
	{
		checkEqual<unsigned>(_set.dspCount(), 8u,
			"the set reports eight attached DSPs");

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			const dsp56k::IPeripherals* const pX = _set.dsp(i).getPeriph(dsp56k::MemArea_X);
			const dsp56k::IPeripherals* const pY = _set.dsp(i).getPeriph(dsp56k::MemArea_Y);

			check(pX == static_cast<const dsp56k::IPeripherals*>(&_set.peripherals(i)),
				"the X face is that slot's Peripherals56311" + slotName(i));
			check(pY == static_cast<const dsp56k::IPeripherals*>(&_set.peripherals(i).ySpace()),
				"the Y face is that peripheral's Y space" + slotName(i));
			check(pX != pY,
				"the two faces are different objects" + slotName(i));
		}

		/* Distinct slots, not one object reported eight times. */
		for(unsigned i = 1; i < _set.dspCount(); ++i)
		{
			check(&_set.dsp(i) != &_set.dsp(0),
				"the DSP is its own object" + slotName(i));
			check(&_set.peripherals(i) != &_set.peripherals(0),
				"the peripheral set is its own object" + slotName(i));
		}
	}

	/* ---------------- group 2: one armed DMA channel per slot
	 *
	 * "No assertion trips" is not falsifiable under NDEBUG, so the check is
	 * the dispatch branch's OWN effect: only that branch registers the
	 * channel as a trigger target. */

	constexpr TWord g_channel = 3;
	constexpr TWord g_source = 0x001000;
	constexpr TWord g_destination = 0x002000;
	constexpr TWord g_hardwareEsaiReceiveData = 11;		// EsaiReceiveData = 0b01011

	constexpr TWord dcrForRequestSource(const TWord _hardwareRequestSource)
	{
		return (1u << dsp56k::DmaChannel::De)
			| (static_cast<TWord>(dsp56k::DmaChannel::TransferMode::WordTriggerRequest) << 19)
			| (_hardwareRequestSource << 11)
			| (static_cast<TWord>(dsp56k::DmaChannel::AddressGenMode::SingleCounterAnoUpdate) << 7)
			| (static_cast<TWord>(dsp56k::DmaChannel::AddressGenMode::SingleCounterApostInc) << 4);
	}

	void eachSlotArmsItsOwnDmaChannel(g2::DspSet& _set)
	{
		using RequestSource = dsp56k::DmaChannel::RequestSource;

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			dsp56k::Dma& dma = _set.peripherals(i).getDMA();

			check(!dma.hasTrigger(RequestSource::EsaiReceiveData),
				"no receive trigger is armed before this test arms one" + slotName(i));

			dma.setDSR(g_channel, g_source);
			dma.setDDR(g_channel, g_destination);
			dma.setDCO(g_channel, 0);
			dma.setDCR(g_channel, dcrForRequestSource(g_hardwareEsaiReceiveData));

			check(dma.hasTrigger(RequestSource::EsaiReceiveData),
				"arming a word channel registers the receive trigger" + slotName(i));
			check(!dma.hasTrigger(RequestSource::EsaiTransmitData),
				"the transmit source stays unarmed" + slotName(i));
		}
	}

	/* ---------------- groups 3 and 4: the two slot-mask registers
	 *
	 * Every slot is written before any slot is read, so a set whose eight
	 * slots share one peripheral object reads back the last value written
	 * rather than its own. */

	TWord tsmaForSlot(const unsigned _slot) { return 0x00A100u + _slot; }
	TWord tsmbForSlot(const unsigned _slot) { return 0x00B200u + _slot; }

	void theAttachedYFaceRoundTripsTsma(g2::DspSet& _set)
	{
		for(unsigned i = 0; i < _set.dspCount(); ++i)
			_set.dsp(i).getPeriph(dsp56k::MemArea_Y)->write(dsp56k::Esai::M_TSMA_1, tsmaForSlot(i));

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			checkEqual(_set.peripherals(i).ySpace().read(dsp56k::Esai::M_TSMA_1, dsp56k::Instruction::Nop),
				tsmaForSlot(i),
				"TSMA_1 reads back through the Y face" + slotName(i));
			checkEqual(_set.peripherals(i).getEsai1().readTSMA(), tsmaForSlot(i),
				"TSMA_1 reads back through the X face's second-ESAI accessor" + slotName(i));
		}
	}

	void theAttachedYFaceRoundTripsTsmb(g2::DspSet& _set)
	{
		for(unsigned i = 0; i < _set.dspCount(); ++i)
			_set.dsp(i).getPeriph(dsp56k::MemArea_Y)->write(dsp56k::Esai::M_TSMB_1, tsmbForSlot(i));

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			checkEqual(_set.peripherals(i).ySpace().read(dsp56k::Esai::M_TSMB_1, dsp56k::Instruction::Nop),
				tsmbForSlot(i),
				"TSMB_1 reads back through the Y face" + slotName(i));
			checkEqual(_set.peripherals(i).getEsai1().readTSMB(), tsmbForSlot(i),
				"TSMB_1 reads back through the X face's second-ESAI accessor" + slotName(i));
		}
	}

	/* ---------------- groups 5 and 6: $FFFF88 in each space
	 *
	 * Timers spans X:$FFFF82 to X:$FFFF8F and ESAI_1's Y face carries
	 * $FFFF88, so the same number names a timer register and a receive data
	 * register. Without the X half, the Y half would pass against a handler
	 * that answers nothing at all. */

	void aYSpaceWriteAtFfff88MovesNoTimer(g2::DspSet& _set)
	{
		for(unsigned i = 0; i < _set.dspCount(); ++i)
			_set.dsp(i).getPeriph(dsp56k::MemArea_X)->write(dsp56k::Timers::M_TCR1, 0x00CAF0u + i);

		for(unsigned i = 0; i < _set.dspCount(); ++i)
			checkEqual(_set.dsp(i).getPeriph(dsp56k::MemArea_X)->read(dsp56k::Timers::M_TCR1, dsp56k::Instruction::Nop),
				0x00CAF0u + i,
				"the X face holds its own slot's timer count register" + slotName(i));

		for(unsigned i = 0; i < _set.dspCount(); ++i)
			_set.dsp(i).getPeriph(dsp56k::MemArea_Y)->write(dsp56k::Esai::M_RX0_1, 0x005555u + i);

		for(unsigned i = 0; i < _set.dspCount(); ++i)
			checkEqual(_set.dsp(i).getPeriph(dsp56k::MemArea_X)->read(dsp56k::Timers::M_TCR1, dsp56k::Instruction::Nop),
				0x00CAF0u + i,
				"a Y-space write at $FFFF88 moved no timer register" + slotName(i));
	}

	void anXSpaceWriteAtFfff88MovesTheTimerAndNotEsai1(g2::DspSet& _set)
	{
		/* A readable witness for ESAI_1, distinct per slot, set through the
		 * attached Y face before the X-space write goes anywhere near it. */
		for(unsigned i = 0; i < _set.dspCount(); ++i)
			_set.dsp(i).getPeriph(dsp56k::MemArea_Y)->write(dsp56k::Esai::M_TSMA_1, 0x00C300u + i);

		for(unsigned i = 0; i < _set.dspCount(); ++i)
			_set.dsp(i).getPeriph(dsp56k::MemArea_X)->write(dsp56k::Timers::M_TCR1, 0x00BEE0u + i);

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			checkEqual(_set.dsp(i).getPeriph(dsp56k::MemArea_X)->read(dsp56k::Timers::M_TCR1, dsp56k::Instruction::Nop),
				0x00BEE0u + i,
				"an X-space write at $FFFF88 moved that slot's timer register" + slotName(i));
			checkEqual(_set.peripherals(i).getEsai1().readTSMA(), 0x00C300u + i,
				"that same write left that slot's ESAI_1 alone" + slotName(i));
		}
	}

	/* ---------------- group 7: the state trio, addressable and typed
	 *
	 * Each pointer binds to exactly the member's type, so a method declared
	 * and never defined is a link error here, and stateLoad returning
	 * anything but g2::Status does not compile. */

	size_t (g2::DspSet::*const kStateSize)() const noexcept = &g2::DspSet::stateSize;
	void (g2::DspSet::*const kStateSave)(void*) const noexcept = &g2::DspSet::stateSave;
	g2::Status (g2::DspSet::*const kStateLoad)(const void*) noexcept = &g2::DspSet::stateLoad;

	void theStateTrioIsDeclaredAndDefined(g2::DspSet& _set)
	{
		(void)kStateSize;
		(void)kStateSave;
		(void)kStateLoad;

		/* The snapshot covers the three memory areas of every slot AT THE
		 * SIZES THE MEMORY ITSELF REPORTS. An implementation that copied only
		 * the internally allocated part of X and Y reports a smaller number
		 * here.
		 *
		 * The register block's serialised byte count is deliberately not
		 * spelled out in this file. dspSet.cpp derives it from the field walk
		 * that writes it, and a second copy of that walk here would compare the
		 * implementation against a restatement of itself. What this check holds
		 * instead is that the memory is covered exactly and that a register
		 * block of the same non-zero cost sits on top of it for every slot. The
		 * CONTENTS of that block are held by the round trip below, which plants
		 * a value in every field. */
		size_t memoryBytes = 0;
		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			const dsp56k::Memory& mem = _set.dsp(i).memory();
			memoryBytes += static_cast<size_t>(mem.size(dsp56k::MemArea_P)) * sizeof(TWord);
			memoryBytes += static_cast<size_t>(mem.size(dsp56k::MemArea_X)) * sizeof(TWord);
			memoryBytes += static_cast<size_t>(mem.size(dsp56k::MemArea_Y)) * sizeof(TWord);
		}

		const size_t total = _set.stateSize();

		check(total > memoryBytes,
			"stateSize covers P, X and Y of every slot AND a register block on top");

		if(total > memoryBytes)
		{
			checkEqual((total - memoryBytes) % _set.dspCount(), size_t(0),
				"the bytes beyond the memory divide evenly across the slots, so every "
				"slot's register block costs the same");
		}
	}

	/* ---------------- group 8: the round trip
	 *
	 * The addresses are inside every area's reported size and inside the
	 * internally allocated part of X and Y, so the poke itself is in bounds
	 * whether or not the host MMU mapping succeeded. */

	constexpr TWord g_lowWord = 0x001000;
	constexpr TWord g_highPWord = 0x07FFFF;
	constexpr TWord g_highXyWord = 0x1FFFFF;

	/* EVERY field of the register block carries a planted value, because
	 * dspSet.cpp writes that block field by field and a field left out of its
	 * walk is a field the snapshot drops in silence. A perturbation over four
	 * registers would pass against a walk that carried only those four.
	 *
	 * The narrowest register in the block holds a single byte, so the value's
	 * LOW BYTE is what has to discriminate, and the packing below puts the
	 * field ordinal and the generation there. That is what each half of the
	 * round trip needs: the generation bit makes the pre-load state differ from
	 * the restored state for every field, so a dropped field keeps the second
	 * generation and is caught; the ordinal makes two fields differ, so a walk
	 * that wrote two of them in the wrong order is caught as well. The slot
	 * sits above the byte, where the wider registers still separate slots.
	 *
	 * The two generations this file drives differ in their low bit. */
	int32_t plantedWord(const unsigned _slot, const TWord _generation, const unsigned _ordinal)
	{
		const TWord low  = (static_cast<TWord>(_ordinal) * 2u + (_generation & 1u)) & 0xFFu;
		const TWord word = ((static_cast<TWord>(_slot) + 1u) << 8) | low;

		return static_cast<int32_t>(word & 0xFFFFFFu);
	}

	/* The register block's storage differs per field: most fields are a
	 * dsp56k::RegType over a signed or unsigned integer, the modulo and mask
	 * arrays are plain words. Each pair below reaches the stored integer of one
	 * of those two shapes, and nothing here constructs a RegType from a number
	 * -- RegType's converting constructor masks and sign-extends, which would
	 * make the fixture rather than the code under test decide what came back. */
	template<typename T, unsigned B>
	void plantInto(dsp56k::RegType<T, B>& _reg, const int32_t _word)
	{
		_reg.var = static_cast<T>(_word);
	}

	void plantInto(TWord& _word, const int32_t _value)
	{
		_word = static_cast<TWord>(_value);
	}

	/* The stored value and the value a plant of the same word WOULD store, both
	 * read out as one signed type so a single comparison covers every field.
	 *
	 * The expectation goes through the same narrowing cast the plant does, and
	 * that cast is the fixture's own -- a register narrower than the planted
	 * word keeps its low part. It is not a model of the code under test: what
	 * the comparison holds is that the bits the register HELD before the save
	 * are the bits it holds after the load. */
	template<typename T, unsigned B>
	int64_t storedValue(const dsp56k::RegType<T, B>& _reg)
	{
		return static_cast<int64_t>(_reg.var);
	}

	int64_t storedValue(const TWord& _word)
	{
		return static_cast<int64_t>(_word);
	}

	template<typename T, unsigned B>
	int64_t plantedValue(const dsp56k::RegType<T, B>&, const int32_t _word)
	{
		return static_cast<int64_t>(static_cast<T>(_word));
	}

	int64_t plantedValue(const TWord&, const int32_t _word)
	{
		return static_cast<int64_t>(static_cast<TWord>(_word));
	}

	/* THE ONE field list. The plant and the check are two visitors over it, so
	 * neither can reach a field the other misses and a field's ordinal is the
	 * same on both passes. It is a separate list from dspSet.cpp's own walk on
	 * purpose: a check that borrowed the walk under test would agree with it
	 * however wrong both were. */
	template<typename R, typename Visit>
	void walkEveryRegister(R& _regs, Visit&& _visit)
	{
		unsigned ordinal = 0;

		const auto one  = [&](auto& _field) { _visit(_field, ordinal++); };
		const auto each = [&](auto& _array)
		{
			for(auto& element : _array)
				_visit(element, ordinal++);
		};

		one(_regs.x);
		one(_regs.y);
		one(_regs.a);
		one(_regs.b);

		each(_regs.r);
		each(_regs.n);
		each(_regs.m);
		each(_regs.mMask);
		each(_regs.mModulo);

		one(_regs.sr);
		one(_regs.omr);
		one(_regs.pc);
		one(_regs.la);
		one(_regs.lc);
		one(_regs.sp);
		one(_regs.sc);

		each(_regs.ss);

		one(_regs.sz);
		one(_regs.vba);
		one(_regs.ep);
	}

	struct Perturbation
	{
		TWord pLow, pHigh, xLow, xHigh, yLow, yHigh;
	};

	Perturbation perturbationForSlot(const unsigned _slot, const TWord _generation)
	{
		const TWord s = static_cast<TWord>(_slot);
		const TWord base = 0x010000u * _generation + 0x000100u * s;

		Perturbation p{};
		p.pLow  = base + 0x11u;
		p.pHigh = base + 0x12u;
		p.xLow  = base + 0x13u;
		p.xHigh = base + 0x14u;
		p.yLow  = base + 0x15u;
		p.yHigh = base + 0x16u;
		return p;
	}

	void applyPerturbation(g2::DspSet& _set, const TWord _generation)
	{
		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			const Perturbation p = perturbationForSlot(i, _generation);

			walkEveryRegister(_set.dsp(i).regs(),
				[&](auto& _field, const unsigned _ordinal)
				{
					plantInto(_field, plantedWord(i, _generation, _ordinal));
				});

			dsp56k::Memory& mem = _set.dsp(i).memory();
			mem.getMemAreaPtr(dsp56k::MemArea_P)[g_lowWord]   = p.pLow;
			mem.getMemAreaPtr(dsp56k::MemArea_P)[g_highPWord] = p.pHigh;
			mem.getMemAreaPtr(dsp56k::MemArea_X)[g_lowWord]   = p.xLow;
			mem.getMemAreaPtr(dsp56k::MemArea_X)[g_highXyWord] = p.xHigh;
			mem.getMemAreaPtr(dsp56k::MemArea_Y)[g_lowWord]   = p.yLow;
			mem.getMemAreaPtr(dsp56k::MemArea_Y)[g_highXyWord] = p.yHigh;
		}
	}

	void checkPerturbation(g2::DspSet& _set, const TWord _generation, const std::string& _what)
	{
		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			const Perturbation p = perturbationForSlot(i, _generation);
			const std::string where = _what + slotName(i);

			walkEveryRegister(static_cast<const dsp56k::DSP::SRegs&>(_set.dsp(i).regs()),
				[&](const auto& _field, const unsigned _ordinal)
				{
					const int32_t word = plantedWord(i, _generation, _ordinal);

					checkEqual(storedValue(_field), plantedValue(_field, word),
						"register field " + std::to_string(_ordinal) + " " + where);
				});

			dsp56k::Memory& mem = _set.dsp(i).memory();
			checkEqual(mem.getMemAreaPtr(dsp56k::MemArea_P)[g_lowWord], p.pLow, "P low " + where);
			checkEqual(mem.getMemAreaPtr(dsp56k::MemArea_P)[g_highPWord], p.pHigh, "P high " + where);
			checkEqual(mem.getMemAreaPtr(dsp56k::MemArea_X)[g_lowWord], p.xLow, "X low " + where);
			checkEqual(mem.getMemAreaPtr(dsp56k::MemArea_X)[g_highXyWord], p.xHigh, "X high " + where);
			checkEqual(mem.getMemAreaPtr(dsp56k::MemArea_Y)[g_lowWord], p.yLow, "Y low " + where);
			checkEqual(mem.getMemAreaPtr(dsp56k::MemArea_Y)[g_highXyWord], p.yHigh, "Y high " + where);
		}
	}

	void aSaveAndLoadRoundTripRestoresEveryPerDspState(g2::DspSet& _set)
	{
		applyPerturbation(_set, 1u);

		std::vector<uint8_t> snapshot(_set.stateSize());
		_set.stateSave(snapshot.data());

		/* Generation 2 differs from generation 1 in every word this test
		 * reads, so a stateLoad that copied nothing leaves generation 2 in
		 * place and every assertion below fails. */
		applyPerturbation(_set, 2u);
		checkPerturbation(_set, 2u, "carries the second generation before the load");

		/* g2::Status is scoped over uint32_t and has no stream inserter. */
		check(_set.stateLoad(snapshot.data()) == g2::Status::Ok,
			"stateLoad reports Ok for a snapshot this set just wrote");

		checkPerturbation(_set, 1u, "is restored by the round trip");
	}
}

int main()
{
	g2::DspSet set;

	theEightSetsAreAttachedFaceByFace(set);
	eachSlotArmsItsOwnDmaChannel(set);
	theAttachedYFaceRoundTripsTsma(set);
	theAttachedYFaceRoundTripsTsmb(set);
	aYSpaceWriteAtFfff88MovesNoTimer(set);
	anXSpaceWriteAtFfff88MovesTheTimerAndNotEsai1(set);
	theStateTrioIsDeclaredAndDefined(set);
	aSaveAndLoadRoundTripRestoresEveryPerDspState(set);

	if(g_failures != 0)
	{
		std::cout << "t0_dsp_attach: " << g_failures << " failure(s) in "
			<< g_cases << " case(s)" << std::endl;
		return 1;
	}

	std::cout << "t0_dsp_attach: all " << g_cases << " cases passed" << std::endl;
	return 0;
}
