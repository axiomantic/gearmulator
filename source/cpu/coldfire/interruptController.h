// The two-tier interrupt controller: the MCF5407 / MCF5307 SIM's centralized interrupt
// controller, on the board side of the mcf5407_set_irq contract. The board
// owns every pending bit and every priority decision; this class is that
// decision. It arbitrates among the internal module sources and the four
// external interrupt pins, computes the single highest-priority winner, and
// presents the whole current state on every change through the present
// callback. The call is idempotent, so the board may invoke it unconditionally
// after every recomputation.

#pragma once

#include <cstdint>

namespace coldfire
{
	// The four external interrupt pins, in the order the manual names them in
	// section 2.3.1. IRQ7 is fixed at level 7; the other three are re-mapped
	// by IRQPAR.
	enum class ExternalPin : int
	{
		Irq7 = 0,
		Irq5 = 1,
		Irq3 = 2,
		Irq1 = 3,
	};

	// The present callback. `level` is MCF5407_IRQ_NONE (0) for none, or 1 to
	// 7. `vector` is the pass-through vector number used when `autovector` is
	// zero; a non-zero `autovector` makes the core use the autovector for
	// `level` and ignore `vector`.
	using InterruptPresentFn = void (*)(void* _user, int _level, uint8_t _vector, int _autovector);

	class InterruptController final
	{
	public:
		// The MBAR-relative offsets this class answers, from the MCF5307
		// User's Manual, sections 8.3.3 and 8.3.4.
		static constexpr uint32_t gAvrOffset    = 0x04Bu;
		static constexpr uint32_t gIcrBase      = 0x04Cu;
		static constexpr uint32_t gIcrCount     = 12u;   // MBAR+$04C..$057
		static constexpr uint32_t gIrqparOffset = 0x006u;

		// The sources the internal control-register block covers. The block
		// carries twelve register slots, of which the last two ($056, $057)
		// are reserved on the MCF5307 and generate no source.
		static constexpr int gInternalSourceCount = 10;

		explicit InterruptController(void* _user = nullptr, InterruptPresentFn _present = nullptr);

		// Register surface. Offset is MBAR-relative. Only $006 (IRQPAR),
		// $04B (AVR) and $04C..$057 (the internal control block) are modelled;
		// any other offset is ignored by both read and write and reads zero.
		void writeRegister(uint32_t _offset, uint8_t _value);
		uint8_t readRegister(uint32_t _offset) const;

		// Source assert/deassert. index is 0..9 (SWT, Timer1, Timer2, MBUS,
		// UART1, UART2, DMA0, DMA1, DMA2, DMA3) for the internal block.
		// Asserting a source whose level is 0 presents nothing by that source,
		// because a level of 0 is an unassigned source. Every change
		// recomputes and presents.
		void setInternalPending(int _index, bool _asserted);
		void setExternalPending(ExternalPin _pin, bool _asserted);

		// Pass-through vector numbers, standing in for the UIVR, SWIVR and DIVR
		// vector registers; the board supplies the firmware values. They only
		// matter when the winning source is not autovectored.
		void setInternalVector(int _index, uint8_t _vector);
		void setExternalVector(ExternalPin _pin, uint8_t _vector);

		void setPresentCallback(void* _user, InterruptPresentFn _present)
		{
			m_user = _user;
			m_present = _present;
			recomputeAndPresent();
		}

		// The last-presented state, readable back by whoever installed the
		// callback.
		int presentedLevel() const { return m_lastLevel; }
		uint8_t presentedVector() const { return m_lastVector; }
		int presentedAutovector() const { return m_lastAutovector; }

	private:
		struct Winner
		{
			bool valid = false;
			int level = 0;
			uint8_t vector = 0;
			int autovector = 0;
		};

		Winner arbitrate() const;
		void recomputeAndPresent();

		void* m_user = nullptr;
		InterruptPresentFn m_present = nullptr;

		uint8_t m_irqpar = 0x00u;
		uint8_t m_avr = 0x00u;
		uint8_t m_icr[gIcrCount] = {};

		bool m_internalPending[gInternalSourceCount] = {};
		bool m_externalPending[4] = {};

		uint8_t m_internalVector[gInternalSourceCount] = {};
		uint8_t m_externalVector[4] = {};

		int m_lastLevel = 0;
		uint8_t m_lastVector = 0;
		int m_lastAutovector = 0;
	};
}
