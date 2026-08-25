/* dspSet.h -- the eight attached DSPs. */

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
	class ChainAdapter;
	class Hdi08Adapter;
	class Hdi08Bridge;

	class DspSet
	{
	public:
		DspSet();

		/* Out of line, because the bridge is incomplete here. Including
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

		/* The snapshot covers the register block of every slot as a struct
		 * copy, plus that slot's P, X and Y memory. It does not cover the
		 * peripherals, because the DSP library carries no save or load member
		 * for a peripheral set, its ESAIs, its Dma, its Timers or its host
		 * port. Restoring those needs new API in the dsp56300 fork.
		 *
		 * It does not cover the bridges either, so stateLoad REFUSES a set
		 * that holds them and answers Status::BridgesAttached. A bridge carries
		 * the landed flag the run gate borrows and a download cursor with no
		 * accessor, and a load that restored the slots alone would leave a
		 * correct program behind a gate that never opens. stateSave has no
		 * return to refuse through, so a snapshot taken from a bridged set is
		 * written and only an unbridged set may take it back. */
		size_t stateSize() const noexcept;
		void   stateSave(void* dst) const noexcept;
		Status stateLoad(const void* src) noexcept;

		/* THE RESET. Task SCH-21 step 3, design section 13.10.5's "zeroes
		 * every emulated memory".
		 *
		 * IT COVERS EXACTLY WHAT THE SNAPSHOT COVERS AND NOTHING MORE, which is
		 * a deliberate pairing rather than a coincidence: every slot's P, X and
		 * Y memory, and that slot's core state through dsp56k::DSP::resetHW.
		 * The peripherals are outside it for the same reason they are outside
		 * stateSave -- the DSP library carries no reset member for a peripheral
		 * set, its ESAIs, its Dma, its Timers or its host port.
		 *
		 * IT DOES NOT DETACH THE BRIDGES AND IT DOES NOT CLEAR A LANDED FLAG.
		 * A bridge is a wire and not emulated memory; tearing it down here
		 * would leave the run gate shut for the life of the program, because
		 * attachHdi08Bridges refuses the second attach that would rebuild it.
		 * THE CONSEQUENCE IS STATED RATHER THAN LEFT TO BE FOUND: a reset set
		 * whose firmware had already landed reports LANDED over zeroed program
		 * memory until the producer downloads again. */
		void reset() noexcept;

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

		/* The one installer, and it is a friend rather than a public setter. A
		 * setter would be a second way to populate this member, and a set holding
		 * bridges nobody attached is a lifetime nobody checked. */
		friend void attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set);

		dsp56k::DefaultMemoryValidator m_memoryValidator;
		std::array<std::unique_ptr<Slot>, 8> m_slots;

		/* Declared after the slots so it is destroyed before them. Each bridge
		 * holds a reference into its slot's peripherals. */
		std::vector<std::unique_ptr<Hdi08Bridge>> m_bridges;
	};

	/* Declared at namespace scope and deliberately not befriended. The
	 * namespace-scope declaration is required: a friend declaration alone is
	 * reachable only through argument-dependent lookup, so a caller naming it
	 * qualified would not find it. Friendship is not, and this is where the
	 * other installer differs rather than matching. This one reaches the set
	 * through the public dspCount() and peripherals() accessors only;
	 * attachHdi08Bridges is a friend because it populates m_bridges and reads
	 * the bound of m_slots.
	 *
	 * A constructor parameter was rejected for the same reason it was there:
	 * every declaration of a DspSet in this tree default-constructs it, the
	 * Board's own member among them, and a parameter breaks all of them where
	 * a free function breaks none. */
	void attachChainCallbacks(ChainAdapter& _adapter, DspSet& _set);
}
