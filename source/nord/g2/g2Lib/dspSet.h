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
	class ChainAdapter;
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

		/* THE DETACH AND ITS EXACT INVERSE. Task SCH-21 step 4, by operator
		 * ruling; section 24.6 row W3-415 records the blocker they answer.
		 *
		 * WHAT THEY ARE FOR. Board's constructor calls attachHdi08Bridges
		 * unconditionally, so EVERY set reachable from a Scheduler holds
		 * bridges, and stateLoad above refuses such a set. Without a detach
		 * Scheduler::stateLoad could never return Ok on any Scheduler built
		 * from a real Board and step 4's round trip was unreachable. The
		 * Scheduler brackets its DSP limb with this pair.
		 *
		 * THEY MOVE THE BRIDGES ASIDE AND BACK; THEY DESTROY NOTHING AND THEY
		 * BUILD NOTHING. That is what makes them EXACTLY inverse rather than
		 * approximately so, and the reason is a borrowed address: Scheduler's
		 * constructor copies programLanded(i) into each DspContext and the run
		 * gate reads through that pointer for the life of the object. A
		 * re-attach that constructed fresh bridges would leave every one of
		 * those pointers dangling -- with no diagnostic anywhere, because the
		 * gate would keep reading whatever now occupies the freed storage.
		 * Moving the same objects back to the same indices is the only shape
		 * with no such failure, and t0_scheduler_state case 1 pins it by the
		 * pointer VALUE at each index rather than by a count.
		 *
		 * BOTH ARE TOTAL. A detach of a detached set and a re-attach of an
		 * attached set each change nothing, so no caller has to test first.
		 *
		 * WHILE DETACHED THE SET REPORTS WHAT IT IS: bridgesAttached() is
		 * false and programLanded() answers NULL for every slot, which is the
		 * run gate's own reading of NOT LANDED.
		 *
		 * THE LANDED FLAG SURVIVES THE ROUND, and that limit is stated rather
		 * than left to be found. The flags live ON the bridges and the bridges
		 * are not touched, so a set whose firmware had landed is still LANDED
		 * after a detach, a load and a re-attach -- over whatever program
		 * memory the load restored. That is right for the case this pair
		 * exists for, a snapshot taken from and returned to the SAME machine,
		 * and it is wrong for an image taken from a machine that had loaded a
		 * different program. Nothing here can tell those two apart; the
		 * version word and the geometry headers guard the SHAPE of an image
		 * and not its provenance. */
		void detachHdi08Bridges() noexcept;
		void reattachHdi08Bridges() noexcept;

		/* TRUE when the set holds bridges, which is exactly the condition
		 * stateLoad refuses on. It is the observable that separates a detach
		 * that reached the set from one that reported success. */
		bool bridgesAttached() const noexcept;

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

		/* THE ONE INSTALLER, AND IT IS A FRIEND RATHER THAN A PUBLIC SETTER. A
		 * setter would be a second way to populate this member, and a set holding
		 * bridges nobody attached is a lifetime nobody checked. */
		friend void attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set);

		dsp56k::DefaultMemoryValidator m_memoryValidator;
		std::array<std::unique_ptr<Slot>, 8> m_slots;

		/* DECLARED AFTER THE SLOTS SO IT IS DESTROYED BEFORE THEM. Each bridge
		 * holds a reference into its slot's peripherals. */
		std::vector<std::unique_ptr<Hdi08Bridge>> m_bridges;

		/* WHERE detachHdi08Bridges PARKS THEM, and the only other place a
		 * bridge this set owns can be. It is declared beside m_bridges and
		 * after the slots for the same lifetime reason: a parked bridge still
		 * holds a reference into its slot's peripherals, so it must be
		 * destroyed before them whichever member happens to hold it. */
		std::vector<std::unique_ptr<Hdi08Bridge>> m_detachedBridges;
	};

	/* DECLARED AT NAMESPACE SCOPE AND DELIBERATELY NOT BEFRIENDED. The
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
	 * a free function breaks none.
	 *
	 * _portOfPosition IS THE CHAIN ORDER AND IT IS REQUIRED. Entry `position`
	 * holds the hardware port -- the SLOT INDEX of this set -- that carries that
	 * chain position. chainOrder.h carries where the order comes from and why it
	 * is not the identity; the short of it is that the firmware chooses which
	 * DSP is the head of the audio chain and it does not choose slot 0. A
	 * position's callbacks installed on the wrong slot put the machine's codec
	 * input and output on DSPs that are not the chain's ends.
	 *
	 * THERE IS NO DEFAULT, AND THAT IS THE POINT. An identity default would be a
	 * chain order embedded in this signature, silently correct-looking and
	 * wrong on the real machine; a caller that has not got the order yet must
	 * say so by not calling this.
	 *
	 * IT RETURNS RATHER THAN ASSERTING. Status::BadChainOrder for an order whose
	 * length is not the slot count, or that is not a permutation of the slots --
	 * an order that named one slot twice would leave another slot unwired and
	 * two positions driving one DSP. THE REFUSAL IS BEFORE THE FIRST INSTALL, so
	 * a refused call changes nothing and the set keeps whatever it wore. */
	Status attachChainCallbacks(ChainAdapter& _adapter, DspSet& _set,
		const std::vector<unsigned>& _portOfPosition);

	/* WHAT AN ESAI CARRIES BEFORE THE CHAIN ORDER IS KNOWN, AND IT IS NOT
	 * NOTHING. dsp56k::Audio's constructor installs ring-buffer callbacks whose
	 * receive half calls waitNotEmpty() -- it BLOCKS until a host supplies a
	 * frame, and no host does on this machine. A port left wearing them stops
	 * the thread that runs its DSP for ever, and the caller's own quantum never
	 * returns to install anything better.
	 *
	 * SO THE PORTS ARE MADE IDLE RATHER THAN LEFT DEFAULT. A receive answers a
	 * CLEARED frame and a transmit is discarded: an unwired chain end reads
	 * silence and its output goes nowhere, which is what an unwired end is.
	 *
	 * BOTH ESAIs OF EVERY SLOT, and the frame index is left alone -- section
	 * 13.10.2's rule that the scheduler's virtual frame index is the only
	 * authoritative one holds here as it does for the real wrappers.
	 *
	 * IT IS TOTAL AND IT OVERWRITES. Calling it on a wired set unwires it. */
	void installIdleChainCallbacks(DspSet& _set);
}
