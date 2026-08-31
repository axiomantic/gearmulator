/* The two ESAI frame calls. */

#include "esaiFrame.h"

#include <cassert>

#include "dsp56kEmu/esai.h"

namespace g2
{
	uint32_t transmitDspFrame(dsp56k::Esai& esai) noexcept
	{
		if(!esai.hasEnabledTransmitters())
			return 0;

		/* The loop counts frames and not slots, and that is the whole reason
		 * the transmit side is written this way. A second caller of execTX
		 * exists -- the guest's own transmit-control-register write -- so the
		 * slot phase can be one slot ahead when this call starts. A
		 * frame-counted loop re-phases itself on the next quantum; a
		 * slot-counted loop would stay one slot out for ever. */
		const uint32_t start = esai.getTxFrameCounter();

		uint32_t slots = 0;

		while(esai.getTxFrameCounter() == start)
		{
			esai.execTX();
			++slots;
		}

		/* An upper bound and not an equality. A transmitter enable costs one
		 * slot, so the quantum after one legitimately returns one slot fewer.
		 *
		 * The default build is Release and DEFINES NDEBUG, so this line is not
		 * in the shipped translation unit. It is kept because a debug build
		 * catches the defect at its source, and t0_esai_frame asserts the same
		 * bound itself, on every quantum, in every build type. An assertion
		 * that fires only in a build nobody runs is not a check. */
		assert(slots <= esai.getTxWordCount() + 1u
			&& "a transmit frame cost more slots than the word count allows");

		return slots;
	}

	uint32_t receiveDspFrame(dsp56k::Esai& esai) noexcept
	{
		if(!esai.hasEnabledReceivers())
			return 0;

		/* A fixed count, and it is exact. The scheduler is the only execRX
		 * caller under this design, so nothing else can move the receive slot
		 * phase between two quanta. */
		const uint32_t slots = esai.getRxWordCount() + 1u;

		for(uint32_t i = 0; i < slots; ++i)
			esai.execRX();

		return slots;
	}
}
