#include "dspSet.h"

#include "chainAdapter.h"
#include "hdi08Adapter.h"
#include "hdi08Bridge.h"

#include "g2/timebase.h"

#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/jitconfig.h"

#include <cstring>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

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

		/* The register block is copied as a struct, not memcpy'd. Every
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
		/* The G2 executes code in the interrupt region as regular jumps, which
		 * is the case jitconfig.h's own comment names for this field. At the
		 * upstream default JitBlock::emit compiles every block entered below
		 * Vba_End under JitOps::FastInterruptMode::Static, which SKIPS the
		 * block's fall-through program-counter seed; braIfBitTestMem then writes
		 * the program counter only on its TAKEN path, so a conditional bit-test
		 * spin down there re-enters its own block for ever.
		 *
		 * It is a read-modify-write and not a fresh JitConfig, so every field
 * this one does not name keeps whatever the library chose for it.
 * n2xdsp.cpp carries the same shape.
 *
 * It sits in the slot constructor because that runs once for each DSP
 * with that DSP already fully built, which is what makes this a
 * per-slot property rather than a property of the set. */
		dsp56k::JitConfig config = dsp.getJit().getConfig();
		config.dynamicFastInterrupts = true;
		dsp.getJit().setConfig(config);
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
		/* The bridges are outside the snapshot, and refusing is the only reading
		 * of that a caller can see. A bridge carries the landed flag the run gate
		 * borrows and dsp56k::DspBoot's download cursor, and neither is written by
		 * stateSave. Restoring the slots alone leaves a slot whose program memory
		 * is right behind a gate that reads NOT landed, attachHdi08Bridges refuses
		 * the second attach that would replace the bridges, and the machine is
		 * then silently dead with no way back.
		 *
		 * Covering the flag instead was refused. It would restore the completed
		 * download and leave a download stopped part way silently wrong, because
		 * the dsp56300 fork carries no accessor for DspBoot's cursor -- the same
		 * reason the peripherals are outside the snapshot. A partial cover fails
		 * quietly; this returns.
		 *
		 * The guard is before the first write, so a refused load changes nothing. */
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

	/* THE DETACH. It MOVES the bridges aside and destroys none of them, which
	 * is what makes reattachHdi08Bridges an exact inverse rather than a rebuild
	 * that happens to produce the same count. dspSet.h carries the whole reason
	 * -- the run gate borrows the ADDRESS programLanded answers -- and
	 * t0_scheduler_state case 1 pins it by that address at every index.
	 *
	 * A DETACHED SET IS INDISTINGUISHABLE FROM A NEVER-ATTACHED ONE at every
	 * public reader: bridgesAttached() is false, programLanded() answers NULL
	 * for every slot because m_bridges is empty, and stateLoad no longer
	 * refuses. That is the point: the guard's condition is m_bridges, so
	 * emptying m_bridges is the whole of the detach.
	 *
	 * TOTAL AT BOTH ENDS. The move-assignment below is well defined on an empty
	 * source, so a second detach is a no-op rather than a discard: an
	 * unconditional `m_detachedBridges = std::move(m_bridges)` on an already
	 * detached set would overwrite the parked bridges with an empty vector and
	 * destroy every one of them. The guard is what stops that, and it is the
	 * one line in this pair whose absence would be silent. */
	void DspSet::detachHdi08Bridges() noexcept
	{
		if(m_bridges.empty())
			return;

		m_detachedBridges = std::move(m_bridges);
		m_bridges.clear();
	}

	/* THE INVERSE. The same objects, in the same order, back at the same
	 * indices. The guard is this call's half of the same no-op rule. */
	void DspSet::reattachHdi08Bridges() noexcept
	{
		if(m_detachedBridges.empty())
			return;

		m_bridges = std::move(m_detachedBridges);
		m_detachedBridges.clear();
	}

	bool DspSet::bridgesAttached() const noexcept
	{
		return !m_bridges.empty();
	}

	/* THE RESET. It walks the SAME slot list and the SAME area list stateSize
	 * and stateSave walk, so a slot or an area added to the snapshot is added
	 * to the reset by the same edit rather than by remembering to.
	 *
	 * resetHW IS THE LIBRARY'S OWN CALL AND NOT A ZEROING OF THE REGISTER
	 * BLOCK. A register file zeroed by memset is not the state a DSP56311
	 * holds after reset -- the status register's reset value is not zero -- so
	 * a hand-written zero would produce a machine no reset ever produces.
	 */
	void DspSet::reset() noexcept
	{
		for(unsigned i = 0; i < dspCount(); ++i)
		{
			Slot& s = slot(i);

			for(const auto area : g_areas)
			{
				const auto bytes = areaByteSize(s.memory, area);
				std::memset(s.memory.getMemAreaPtr(area), 0, bytes);
			}

			s.dsp.resetHW();
		}
	}

	/* THE INSTALL LIVES HERE RATHER THAN IN hdi08Bridge.cpp because this is the
	 * construction point that holds both ends of the wire.
	 *
	 * The set keeps them rather than the caller. The run gate reads a flag that
	 * lives on a bridge, so a caller-owned vector would leave the set unable to
	 * answer for its own slots; and a bridge outliving nothing but a local is a
	 * port driving a backlog its owner has dropped. */
	void attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set)
	{
		/* The slot count and the port count are two independent constants, and
		 * nothing but this line ties them. The loop below indexes the adapter with
		 * a slot index, and Hdi08Adapter::port states that it asserts no bounds, so
		 * a slot array that outgrew the port array would read past it and report
		 * nothing. This assertion is inside the friend because m_slots is private
		 * and its bound is the only compile-time spelling of the slot count. */
		static_assert(std::tuple_size<decltype(DspSet::m_slots)>::value
				== static_cast<size_t>(g_hdi08PortCount),
			"the DSP set holds one slot for each HDI08 host port. attachHdi08Bridges "
			"indexes the adapter with a slot index and Hdi08Adapter::port asserts no "
			"bounds.");

		/* A second attach is refused rather than made re-entrant. Replacing the
		 * bridges would destroy the objects whose addresses the run gate already
		 * borrowed through programLanded, and a borrowed pointer has no way to
		 * learn that. Of the two failures only this one is visible. */
		if(!_set.m_bridges.empty())
			throw std::logic_error("attachHdi08Bridges: the set already holds bridges");

		/* A DETACHED SET ALREADY HOLDS ITS BRIDGES; IT IS ONLY NOT WEARING
		 * THEM. Testing m_bridges alone would let this call build a second
		 * bridge for every host port while the parked ones stayed alive, and
		 * the two would then drive the same port. reattachHdi08Bridges is the
		 * only way back from a detach and this refusal is what says so. */
		if(!_set.m_detachedBridges.empty())
			throw std::logic_error("attachHdi08Bridges: the set holds detached bridges -- "
				"reattachHdi08Bridges is the only way back from a detach");

		_set.m_bridges.reserve(_set.dspCount());

		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			_set.m_bridges.emplace_back(new Hdi08Bridge(_adapter.port(static_cast<int>(i)),
				_set.dsp(i), _set.peripherals(i).getHDI08()));
		}
	}

	/* The install lives here for the reason attachHdi08Bridges does: this is
	 * the construction point that holds both ends of the wire. The adapter
	 * keeps only borrowed ESAI pointers and hands out callables that borrow
	 * it, so nothing is owned on either side and nothing is stored here. */
	void attachChainCallbacks(ChainAdapter& _adapter, DspSet& _set)
	{
		/* Every position is attached before the first factory runs, which is
		 * the order chainAdapter.h states: a position's transmit wrapper reads
		 * the ESAI it was given from its first fire, and a wrapper produced
		 * ahead of the attach carries a null one for the whole run. */
		for(unsigned i = 0; i < _set.dspCount(); ++i)
		{
			dsp56k::Peripherals56311& p = _set.peripherals(i);
			_adapter.attachEsai(i, p.getEsai(), p.getEsai1());
		}

		/* The pair, and not setCallback. mqLib, xtLib and nord/n2x install a
		 * single listener through Audio::setCallback; the chain needs the two
		 * directions separately, which is what the adapter's per-direction
		 * callback factories return. */
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
