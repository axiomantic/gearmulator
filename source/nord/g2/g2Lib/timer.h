// One MCF5307 general-purpose timer. The part carries two identical modules,
// one at MBAR+$140 and one at MBAR+$180; this class is one of them.
//
// The register block, offsets relative to the module base. MCF5307 UM
// section 9.4.1 Table 9-5:
//
//     +$00  TMR   16-bit  read/write   timer mode register
//     +$04  TRR   16-bit  read/write   timer reference register
//     +$08  TCR   16-bit  read only    timer capture register
//     +$0C  TCN   16-bit  read/write   timer counter
//     +$11  TER    8-bit  write-1-clr  timer event register
//
// TMR:
//
//     bits 15:8  PS      prescaler -- the counter advances once per PS + 1
//                        input clocks
//     bits  7:6  CE      capture edge and interrupt enable
//     bit     5  OM      output mode
//     bit     4  ORI     output reference interrupt enable
//     bit     3  FRR     free run / restart: set restarts the counter from
//                        zero on a reference match, clear runs it on
//     bits  2:1  CLK     input clock source
//     bit     0  RST     0 disables and resets the timer, 1 enables it
//
// TER: bit 1 REF, bit 0 CAP. Both are cleared by writing a one and are
// unchanged by a zero, which is why the firmware's handler at 0x30001894
// writes 2 to MBAR+$191 rather than 0.
//
// The period is (PS + 1) * (TRR + 1) input clocks. The reference match is
// evaluated against the counter before the tick increments it, so a counter
// standing at TRR matches on its next tick: TRR = 4 matches on the fifth tick
// and not on the fourth.
//
// Capture is not modelled. TCR reads zero and TER[CAP] is never set, because
// no G2 signal reaches the timer input pins. The bit is still
// write-one-to-clear so the firmware's handler behaves the same whichever bit
// it names.

#pragma once

#include <cstdint>

#include "interruptController.h"

namespace g2
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
		// index -- so the firmware's ICR at MBAR+$04E, which is $04C + 2, is
		// timer 2's.
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
		explicit Timer(int _interruptIndex, InterruptController* _interrupts = nullptr);

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

		// True when this block offset is one of the five registers above. A
		// byte the block covers but no register claims is reserved and stays
		// with the SIM's own storage.
		static bool coversByte(uint32_t _blockOffset);

		// The byte surface the SIM's decode drives. The part is big-endian, so
		// byte 0 of a 16-bit register holds its most significant eight bits.
		uint8_t readByte(uint32_t _blockOffset) const;
		void writeByte(uint32_t _blockOffset, uint8_t _value);

		// Advance the module by _inputClocks input clocks. The prescaler is
		// carried across calls, so advancing by one clock a hundred times is
		// the same count as advancing by a hundred once.
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

		// Input clocks seen since the last counter tick. Not a register the
		// programming model carries; the prescaler's own state.
		uint32_t m_prescaler = 0;

		bool m_interruptAsserted = false;
	};
}
