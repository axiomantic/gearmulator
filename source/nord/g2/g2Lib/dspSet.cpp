#include "dspSet.h"

#include "chainAdapter.h"
#include "hdi08Adapter.h"
#include "hdi08Bridge.h"

#include "g2/timebase.h"

#include <cstring>
#include <new>
#include <stdexcept>
#include <tuple>

namespace g2
{
	namespace
	{
		/* The memory shape every DSP fixture in this tree already builds. */
		constexpr dsp56k::TWord g_memSizeP = 0x080000;
		constexpr dsp56k::TWord g_memSizeXY = 0x800000;
		constexpr dsp56k::TWord g_bridgedMemoryAddress = 0x200000;

		/* timebase.h is the only definition site of both, and it states this
		 * derivation: the second ESAI runs the frame rate divided by the
		 * second bus's frame divider. */
		constexpr uint32_t g_secondBusFrameRateHz = G2_FRAME_RATE_HZ / G2_SECOND_BUS_FRAME_DIVIDER;

		constexpr dsp56k::EMemArea g_areas[] =
		{
			dsp56k::MemArea_P, dsp56k::MemArea_X, dsp56k::MemArea_Y
		};

		/* The register block is COPIED AS A STRUCT, not memcpy'd. Every
		 * register in it is a RegType, which declares its own copy
		 * constructor, so the block is not trivially copyable and a memcpy of
		 * it is undefined however plain the data underneath. */
		using Regs = dsp56k::DSP::SRegs;

		/* Placement-constructing Regs inside the caller's buffer needs that
		 * buffer aligned for it. Every area contributes a whole number of
		 * TWords and sizeof(Regs) is a multiple of its own alignment, so each
		 * slot advances the cursor by a multiple of alignof(Regs) and only
		 * the caller's pointer has to carry the alignment. */
		static_assert(alignof(Regs) <= alignof(std::max_align_t),
			"a snapshot buffer from operator new cannot be aligned for the register block");
		static_assert(sizeof(dsp56k::TWord) % alignof(Regs) == 0
			|| alignof(Regs) % sizeof(dsp56k::TWord) == 0,
			"a memory area's byte count can leave the cursor misaligned for the next slot");

		size_t areaByteSize(const dsp56k::Memory& _memory, const dsp56k::EMemArea _area) noexcept
		{
			return static_cast<size_t>(_memory.size(_area)) * sizeof(dsp56k::TWord);
		}
	}

	DspSet::Slot::Slot(const dsp56k::IMemoryValidator& _validator, const uint32_t _secondBusFrameRateHz)
		: memory(_validator, g_memSizeP, g_memSizeXY, g_bridgedMemoryAddress)
		, peripherals(_secondBusFrameRateHz)
		, dsp(memory, &peripherals, &peripherals.ySpace())
	{
	}

	DspSet::DspSet()
	{
		for(auto& slot : m_slots)
			slot = std::make_unique<Slot>(m_memoryValidator, g_secondBusFrameRateHz);
	}

	DspSet::~DspSet() = default;

	unsigned DspSet::dspCount() const noexcept
	{
		return static_cast<unsigned>(m_slots.size());
	}

	/* A const unique_ptr yields a NON-const pointee, which is what lets the
	 * const state methods reach dsp56k::Memory::getMemAreaPtr -- the library
	 * offers no const overload of it. */
	DspSet::Slot& DspSet::slot(const unsigned _index) const noexcept
	{
		return *m_slots[_index];
	}

	dsp56k::DSP& DspSet::dsp(const unsigned _index) noexcept
	{
		return slot(_index).dsp;
	}

	const dsp56k::DSP& DspSet::dsp(const unsigned _index) const noexcept
	{
		return slot(_index).dsp;
	}

	dsp56k::Peripherals56311& DspSet::peripherals(const unsigned _index) noexcept
	{
		return slot(_index).peripherals;
	}

	const dsp56k::Peripherals56311& DspSet::peripherals(const unsigned _index) const noexcept
	{
		return slot(_index).peripherals;
	}

	const bool* DspSet::programLanded(const unsigned _index) const noexcept
	{
		if(_index >= m_bridges.size())
			return nullptr;

		return m_bridges[_index]->programLanded();
	}

	size_t DspSet::stateSize() const noexcept
	{
		size_t total = 0;

		for(unsigned i = 0; i < dspCount(); ++i)
		{
			const dsp56k::Memory& memory = slot(i).memory;

			total += sizeof(Regs);

			for(const auto area : g_areas)
				total += areaByteSize(memory, area);
		}

		return total;
	}

	void DspSet::stateSave(void* const _dst) const noexcept
	{
		auto* cursor = static_cast<uint8_t*>(_dst);

		for(unsigned i = 0; i < dspCount(); ++i)
		{
			Slot& s = slot(i);

			new (cursor) Regs(s.dsp.regs());
			cursor += sizeof(Regs);

			for(const auto area : g_areas)
			{
				const auto bytes = areaByteSize(s.memory, area);
				std::memcpy(cursor, s.memory.getMemAreaPtr(area), bytes);
				cursor += bytes;
			}
		}
	}

	Status DspSet::stateLoad(const void* const _src) noexcept
	{
		/* THE BRIDGES ARE OUTSIDE THE SNAPSHOT, AND REFUSING IS THE ONLY READING
		 * OF THAT A CALLER CAN SEE. A bridge carries the landed flag the run gate
		 * borrows and dsp56k::DspBoot's download cursor, and neither is written by
		 * stateSave. Restoring the slots alone leaves a slot whose program memory
		 * is right behind a gate that reads NOT landed, attachHdi08Bridges refuses
		 * the second attach that would replace the bridges, and the machine is
		 * then silently dead with no way back.
		 *
		 * COVERING THE FLAG INSTEAD WAS REFUSED. It would restore the completed
		 * download and leave a download stopped part way silently wrong, because
		 * the dsp56300 fork carries no accessor for DspBoot's cursor -- the same
		 * reason the peripherals are outside the snapshot. A partial cover fails
		 * quietly; this returns.
		 *
		 * THE GUARD IS BEFORE THE FIRST WRITE, so a refused load changes nothing. */
		if(!m_bridges.empty())
			return Status::BridgesAttached;

		const auto* cursor = static_cast<const uint8_t*>(_src);

		for(unsigned i = 0; i < dspCount(); ++i)
		{
			Slot& s = slot(i);

			s.dsp.regs() = *reinterpret_cast<const Regs*>(cursor);
			cursor += sizeof(Regs);

			for(const auto area : g_areas)
			{
				const auto bytes = areaByteSize(s.memory, area);
				std::memcpy(s.memory.getMemAreaPtr(area), cursor, bytes);
				cursor += bytes;
			}
		}

		return Status::Ok;
	}

	/* THE INSTALL LIVES HERE RATHER THAN IN hdi08Bridge.cpp because this is the
	 * construction point that holds both ends of the wire.
	 *
	 * THE SET KEEPS THEM RATHER THAN THE CALLER. The run gate reads a flag that
	 * lives on a bridge, so a caller-owned vector would leave the set unable to
	 * answer for its own slots; and a bridge outliving nothing but a local is a
	 * port driving a backlog its owner has dropped. */
	void attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set)
	{
		/* THE SLOT COUNT AND THE PORT COUNT ARE TWO INDEPENDENT CONSTANTS, AND
		 * NOTHING BUT THIS LINE TIES THEM. The loop below indexes the adapter with
		 * a slot index, and Hdi08Adapter::port states that it asserts no bounds, so
		 * a slot array that outgrew the port array would read past it and report
		 * nothing. This assertion is inside the friend because m_slots is private
		 * and its bound is the only compile-time spelling of the slot count. */
		static_assert(std::tuple_size<decltype(DspSet::m_slots)>::value
				== static_cast<size_t>(g_hdi08PortCount),
			"the DSP set holds one slot for each HDI08 host port. attachHdi08Bridges "
			"indexes the adapter with a slot index and Hdi08Adapter::port asserts no "
			"bounds.");

		/* A SECOND ATTACH IS REFUSED RATHER THAN MADE RE-ENTRANT. Replacing the
		 * bridges would destroy the objects whose addresses the run gate already
		 * borrowed through programLanded, and a borrowed pointer has no way to
		 * learn that. Of the two failures only this one is visible. */
		if(!_set.m_bridges.empty())
			throw std::logic_error("attachHdi08Bridges: the set already holds bridges");

		_set.m_bridges.reserve(_set.dspCount());

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			_set.m_bridges.emplace_back(new Hdi08Bridge(_adapter.port(static_cast<int>(i)),
				_set.dsp(i), _set.peripherals(i).getHDI08()));
		}
	}

	/* THE INSTALL LIVES HERE FOR THE REASON attachHdi08Bridges DOES: this is
	 * the construction point that holds both ends of the wire. The adapter
	 * keeps only borrowed ESAI pointers and hands out callables that borrow
	 * it, so nothing is owned on either side and nothing is stored here. */
	void attachChainCallbacks(ChainAdapter& _adapter, DspSet& _set)
	{
		/* EVERY POSITION IS ATTACHED BEFORE THE FIRST FACTORY RUNS, which is
		 * the order chainAdapter.h states: a position's transmit wrapper reads
		 * the ESAI it was given from its first fire, and a wrapper produced
		 * ahead of the attach carries a null one for the whole run. */
		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			dsp56k::Peripherals56311& p = _set.peripherals(i);
			_adapter.attachEsai(i, p.getEsai(), p.getEsai1());
		}

		/* THE PAIR AND NOT setCallback. mqLib, xtLib and nord/n2x install a
		 * single listener through Audio::setCallback; the chain needs the two
		 * directions separately, which is what the adapter's four factories
		 * return. */
		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			dsp56k::Peripherals56311& p = _set.peripherals(i);

			p.getEsai().setReadRxCallback(_adapter.audioRxCallback(i));
			p.getEsai().setWriteTxCallback(_adapter.audioTxCallback(i));

			p.getEsai1().setReadRxCallback(_adapter.secondRxCallback(i));
			p.getEsai1().setWriteTxCallback(_adapter.secondTxCallback(i));
		}
	}
}
