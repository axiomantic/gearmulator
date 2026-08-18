/* dspSet.h -- the eight attached DSPs. Design 11.1, 13.10.3. */

#pragma once

#include "status.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals56311.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace g2
{
	class Hdi08Adapter;
	class Hdi08Bridge;

	class DspSet
	{
	public:
		DspSet();

		/* OUT OF LINE, BECAUSE THE BRIDGE IS INCOMPLETE HERE. Including
		 * hdi08Bridge.h would put the host-side `mc68k::Hdi08` into the include
		 * closure of every consumer of this header. */
		~DspSet();

		unsigned dspCount() const noexcept;

		dsp56k::DSP&       dsp(unsigned index) noexcept;
		const dsp56k::DSP& dsp(unsigned index) const noexcept;

		dsp56k::Peripherals56311&       peripherals(unsigned index) noexcept;
		const dsp56k::Peripherals56311& peripherals(unsigned index) const noexcept;

		/* The scheduler's run gate borrows this for the whole run, so it is a
		 * pointer into the owning bridge and not a copied bool. NULL for a slot
		 * with no bridge attached, which is the gate's own reading of "not
		 * landed" and needs no agreement between the two sides. */
		const bool* programLanded(unsigned index) const noexcept;

		/* The trio BRD-21's Board and CHN-5's ChainAdapter already carry.
		 * Those two return void from stateLoad because g2::Status did not
		 * exist when they were written; this one returns the type.
		 *
		 * WHAT THE SNAPSHOT COVERS: the register block of every slot as a
		 * struct copy, plus that slot's P, X and Y memory. IT DOES NOT COVER
		 * THE PERIPHERALS, because the DSP library carries no save or load
		 * member for a peripheral set, its ESAIs, its Dma, its Timers or its
		 * host port. Restoring those needs new API in the dsp56300 fork.
		 *
		 * IT DOES NOT COVER THE BRIDGES EITHER, so stateLoad REFUSES a set
		 * that holds them and answers Status::BridgesAttached. A bridge carries
		 * the landed flag the run gate borrows and a download cursor with no
		 * accessor, and a load that restored the slots alone would leave a
		 * correct program behind a gate that never opens. stateSave has no
		 * return to refuse through, so a snapshot taken from a bridged set is
		 * written and only an unbridged set may take it back. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

	private:
		struct Slot
		{
			Slot(const dsp56k::IMemoryValidator& validator, uint32_t secondBusFrameRateHz);

			/* Declaration order is construction order and the DSP takes the
			 * other two by reference and by address. */
			dsp56k::Memory           memory;
			dsp56k::Peripherals56311 peripherals;
			dsp56k::DSP              dsp;
		};

		Slot& slot(unsigned index) const noexcept;

		/* THE ONE INSTALLER, AND IT IS A FRIEND RATHER THAN A PUBLIC SETTER. A
		 * setter would be a second way to populate this member, and a set holding
		 * bridges nobody attached is a lifetime nobody checked. */
		friend void attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set);

		dsp56k::DefaultMemoryValidator m_memoryValidator;
		std::array<std::unique_ptr<Slot>, 8> m_slots;

		/* DECLARED AFTER THE SLOTS SO IT IS DESTROYED BEFORE THEM. Each bridge
		 * holds a reference into its slot's peripherals. */
		std::vector<std::unique_ptr<Hdi08Bridge>> m_bridges;
	};
}
