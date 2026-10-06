// One MCF5407 / MCF5307 general-purpose timer. The part carries two identical modules,
// one at MBAR+$140 and one at MBAR+$180; this class is one of them.

#pragma once

#include <cstdint>

#include "interruptController.h"

namespace coldfire
{
	class Timer final
	{
	public:
		// The two module bases, MBAR-relative.
		static constexpr uint32_t gTimer1Base = 0x140u;
		static constexpr uint32_t gTimer2Base = 0x180u;

		// The register offsets within one module, and the size of the block
		// the module answers.
		static constexpr uint32_t gTmrOffset = 0x00u;
		static constexpr uint32_t gTrrOffset = 0x04u;
		static constexpr uint32_t gTcrOffset = 0x08u;
		static constexpr uint32_t gTcnOffset = 0x0Cu;
		static constexpr uint32_t gTerOffset = 0x11u;
		static constexpr uint32_t gBlockSize = 0x12u;

		// The internal interrupt sources of the two timers. ICR1 is timer 1
		// and ICR2 is timer 2, and the controller's source index is the ICR
		// index.
		static constexpr int gTimer1InterruptIndex = 1;
		static constexpr int gTimer2InterruptIndex = 2;

		// TMR bit positions.
		static constexpr uint16_t gTmrRst = 0x0001u;
		static constexpr uint16_t gTmrFrr = 0x0008u;
		static constexpr uint16_t gTmrOri = 0x0010u;
		static constexpr int      gTmrPrescalerShift = 8;

		// TER bit positions.
		static constexpr uint8_t gTerCap = 0x01u;
		static constexpr uint8_t gTerRef = 0x02u;

		// The controller to assert on, or nullptr for a standalone unit.
		explicit Timer(int _interruptIndex = gTimer1InterruptIndex, InterruptController* _interrupts = nullptr);

		void setInterruptController(InterruptController* _interrupts);

		uint16_t tmr() const { return m_tmr; }
		uint16_t trr() const { return m_trr; }
		uint16_t tcr() const { return m_tcr; }
		uint16_t tcn() const { return m_tcn; }
		uint8_t  ter() const { return m_ter; }

		void writeTmr(uint16_t _value);
		void writeTrr(uint16_t _value);
		void writeTcn(uint16_t _value);
		void writeTer(uint8_t _value);

		// True when this block offset is one of the five registers above.
		static bool coversByte(uint32_t _blockOffset);

		// The byte surface the SIM's decode drives.
		uint8_t readByte(uint32_t _blockOffset) const;
		void writeByte(uint32_t _blockOffset, uint8_t _value);

		// Advance the module by _inputClocks input clocks.
		void advance(uint32_t _inputClocks);

	private:
		void tick();
		void recomputeInterrupt();
		void setPending(bool _asserted);

		int m_interruptIndex;
		InterruptController* m_interrupts;

		uint16_t m_tmr = 0x0000u;
		uint16_t m_trr = 0xffffu;
		uint16_t m_tcr = 0x0000u;
		uint16_t m_tcn = 0x0000u;
		uint8_t  m_ter = 0x00u;

		uint32_t m_prescaler = 0;
		bool m_interruptAsserted = false;
	};
}
