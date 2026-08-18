#include "dspSet.h"

#include "hdi08Adapter.h"
#include "hdi08Bridge.h"

#include "g2/timebase.h"

#include <cstring>
#include <new>

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
	 * construction point that holds both ends of the wire. */
	std::vector<std::unique_ptr<Hdi08Bridge>> attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set)
	{
		std::vector<std::unique_ptr<Hdi08Bridge>> bridges;
		bridges.reserve(_set.dspCount());

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			bridges.emplace_back(new Hdi08Bridge(_adapter.port(static_cast<int>(i)),
				_set.peripherals(i).getHDI08()));
		}

		return bridges;
	}
}
