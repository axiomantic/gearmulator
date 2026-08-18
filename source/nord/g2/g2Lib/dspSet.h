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

namespace g2
{
	class DspSet
	{
	public:
		DspSet();

		unsigned dspCount() const noexcept;

		dsp56k::DSP&       dsp(unsigned index) noexcept;
		const dsp56k::DSP& dsp(unsigned index) const noexcept;

		dsp56k::Peripherals56311&       peripherals(unsigned index) noexcept;
		const dsp56k::Peripherals56311& peripherals(unsigned index) const noexcept;

		/* The trio BRD-21's Board and CHN-5's ChainAdapter already carry.
		 * Those two return void from stateLoad because g2::Status did not
		 * exist when they were written; this one returns the type.
		 *
		 * WHAT THE SNAPSHOT COVERS: the register block of every slot as a
		 * struct copy, plus that slot's P, X and Y memory. IT DOES NOT COVER
		 * THE PERIPHERALS, because the DSP library carries no save or load
		 * member for a peripheral set, its ESAIs, its Dma, its Timers or its
		 * host port. Restoring those needs new API in the dsp56300 fork. */
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

		dsp56k::DefaultMemoryValidator m_memoryValidator;
		std::array<std::unique_ptr<Slot>, 8> m_slots;
	};
}
