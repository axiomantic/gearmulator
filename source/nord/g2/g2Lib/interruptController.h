// Task BRD-3. The two-tier interrupt controller.
//
// Plan section 13.1, BRD-3. Design sections 5.2.2, 6.4, 17 row 7.25.
//
// WHAT THIS FILE IS. The MCF5307 SIM's centralized interrupt controller, on
// the board side of the mcf5307_set_irq contract. The board owns every pending
// bit and every priority decision; this class is that decision. It arbitrates
// among the internal module sources and the four external interrupt pins,
// computes the single highest-priority winner, and presents the whole current
// state on every change through the present callback, exactly as design
// section 5.2.2 specifies. The call is idempotent, so the board may invoke it
// unconditionally after every recomputation.
//
// CLEAN-ROOM. The two-tier arbitration is written from the MCF5307 User's
// Manual and nothing is harvested. No file of MegabytePhreak/qemu-mcf5307 was
// opened for this task. Every fact below carries its manual section; the full
// provenance record is at the head of interruptController.cpp.
//
// THE FACTS THIS CLASS MODELS, AND WHERE EACH IS READ:
//
//   * The internal interrupt control registers at MBAR+$04C..$057, one per
//     internal module source, each an 8-bit register whose bit 7 is AVEC and
//     whose IL[2:0] (bits 4:2) and IP[1:0] (bits 1:0) carry the level and the
//     within-level priority. MCF5307 UM section 8.3.4, Table 8-2 (memory map)
//     and the ICR programming model on pp. 8-5..8-6.
//   * The shared Autovector Control Register AVR at MBAR+$04B, a bitmask over
//     levels 1..7 that autovectors the external pin at each level. MCF5307 UM
//     section 8.3.4, p. 8-7 for the bit layout, and Table B-1 for the ADDRESS:
//     `MBAR+$04B AVCR 8 AUTOVECTOR CONTROL REGISTER $00 R/W`. Table 8-1's
//     $048 row is four byte columns whose first three are Reserved, so $048 is
//     the LONGWORD GROUP BASE and $04B is the register byte. Table B-1 gives
//     $048, $049 and $04A no row at all, so this class answers none of them --
//     and both G2 firmware images write the byte at $1000004B.
//   * IRQPAR at MBAR+$006, which re-maps the external pins: IRQ5 to level 5 or
//     4, IRQ3 to level 6 or 3, IRQ1 to level 1 or 2. IRQ7 is always level 7.
//     MCF5307 UM section 8.3.4.1 and Table 8-4, pp. 8-9..8-10.
//   * Only four external interrupt pins exist: IRQ7, IRQ5, IRQ3 and IRQ1.
//     MCF5307 UM section 2.3.1.
//   * The two-tier priority order within a level, from Table 8-3: an internal
//     source with IP=11 ranks highest, then IP=10, then the external pin at
//     that level, then IP=01, then IP=00. pp. 8-6..8-7.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG.

#pragma once

#include <cstdint>

namespace g2
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

	// The present callback. `level` is MCF5307_IRQ_NONE (0) for none, or 1 to
	// 7. `vector` is the pass-through vector number used when `autovector` is
	// zero; a non-zero `autovector` makes the core use the autovector for
	// `level` and ignore `vector`.
	using InterruptPresentFn = void (*)(void* _user, int _level, uint8_t _vector, int _autovector);

	class InterruptController final
	{
	public:
		// The MBAR-relative offsets this class answers. All three are facts
		// from the MCF5307 User's Manual, sections 8.3.3 and 8.3.4.
		static constexpr uint32_t gAvrOffset   = 0x04Bu;
		static constexpr uint32_t gIcrBase     = 0x04Cu;
		static constexpr uint32_t gIcrCount    = 12u;   // MBAR+$04C..$057
		static constexpr uint32_t gIrqparOffset = 0x006u;

		// The sources the internal control-register block covers. The block
		// carries twelve register slots, of which the last two ($056, $057)
		// are reserved on the MCF5307 and generate no source. Design section
		// 5.2.2 and plan BRD-3 state the arbiter's source set.
		static constexpr int gInternalSourceCount = 10;

		// InterruptController(_user, _present): _present is called with the
		// whole current state after every mutation. It is never called with a
		// null function pointer.
		InterruptController(void* _user, InterruptPresentFn _present);

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

		// Pass-through vector numbers. These stand in for the UIVR, SWIVR and
		// DIVR vector registers design section 5.2.2 names; the firmware
		// values arrive from SPK-13 and are supplied by the board. They only
		// matter when the winning source is not autovectored.
		void setInternalVector(int _index, uint8_t _vector);
		void setExternalVector(ExternalPin _pin, uint8_t _vector);

		// The last-presented state, so that a test that installs the callback
		// can read back what was presented as well as record it.
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

		void* m_user;
		InterruptPresentFn m_present;

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
