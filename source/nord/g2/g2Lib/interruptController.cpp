// Task BRD-3. The two-tier interrupt controller.
//
// Plan section 13.1, BRD-3. Design sections 5.2.2, 6.4, 17 row 7.25.
//
// ---------------------------------------------------------------------------
// PROVENANCE. Plan section 13.1 requires a record of what this task took and
// where each fact was read. This is that record.
//
// NO FILE OF MegabytePhreak/qemu-mcf5307 WAS OPENED FOR THIS TASK, and no
// value below was checked against it. AGENTS.md section 4.2 forbids taking
// expression from that repository: a corrected derivative of its priority
// formula would still be a derivative, and this controller is written from the
// manual alone.
//
// ---------------------------------------------------------------------------
// THE MANUAL. MCF5307 ColdFire Integrated Microprocessor User's Manual,
// Motorola, 1998. 456 pages, 27,240,768 bytes, sha256
// 86cbcc8c9caa933fe10275a975a78d914df86771df9f0bc22d03de8b1aff91fa. The same
// copy task BRD-2's sim.cpp documents. Every citation below was opened and
// read in it.
//
// SOURCE: MCF5307 User's Manual, section 8 "System Integration Module".
//
//   * The two-tier structure of the controller and the fact that every
//     interrupt source has its own ICR: section 8.3.3 "Interrupt Controller"
//     and section 8.3.4 "Interrupt Registers", pp. 8-5..8-8.
//   * The internal control registers at MBAR+$04C..$057 and which module each
//     owns: Table 8-2, p. 8-5. ICR10 and ICR11 at $056 and $057 are Reserved
//     and generate no source; the model carries the twelve storage slots and
//     connects only the first ten to sources.
//   * The ICR layout: bit 7 AVEC, IL[2:0] at bits 4:2, IP[1:0] at bits 1:0,
//     pp. 8-5..8-6. AVEC=0 means the source returns a vector during the
//     interrupt-acknowledge cycle; AVEC=1 means the SIM generates the
//     autovector.
//   * The AVR at MBAR+$048, AVEC[7:1], a bitmask over levels that
//     autovectors the external pin at each level: section 8.3.4, p. 8-7.
//   * IRQPAR at MBAR+$006 and Table 8-4: IRQ5 is level 5 (IRQPAR[2]=0) or 4
//     (IRQPAR[2]=1); IRQ3 is level 3 (IRQPAR[1]=0) or 6 (IRQPAR[1]=1); IRQ1 is
//     level 1 (IRQPAR[0]=0) or 2 (IRQPAR[0]=1); IRQ7 is always level 7.
//     Section 8.3.4.1 and Table 8-4, pp. 8-9..8-10.
//   * THE TWO-TIER PRIORITY ORDER, Table 8-3, pp. 8-6..8-7. Tier 1 is the
//     interrupt level, level 7 highest and level 1 lowest. Tier 2 is the
//     within-level order the table fixes: an internal source with IP=11 ranks
//     highest, then IP=10, then the external pin at that level, then IP=01,
//     then IP=00. "There are 35 possible priority levels, including internal
//     and external interrupts."
//   * Only four external interrupt pins exist -- IRQ7, IRQ5, IRQ3 and IRQ1:
//     section 2.3.1.
//
// DESIGN SECTIONS, for the interface shape rather than the arbitration.
//
//   * Design section 5.2.2: the board owns every pending bit and every
//     priority decision, presents the whole current state on every change,
//     the call is idempotent, levels 1..6 are level-sensitive and level 7 is
//     edge-triggered (a core property, not this controller's), and the
//     autovector argument follows the AVEC/AVR bit the firmware programmed.
//   * Design section 6.4: the SIM on the board carries the two-tier interrupt
//     controller.
//   * Design row 7.25: the board owns every pending bit and presents its whole
//     current state on each change; a level source drops when the device model
//     clears its own condition and the board recomputes.
//
// THE ONE EXPLICIT TIE-UNTIE IN THE ARBITRATION. The manual fixes the order of
// every pair this controller can present EXCEPT two internal sources that are
// pending at the same level AND the same IP[1:0] -- the pair Table 8-3's
// footnote tells the programmer not to create. For that pair alone the
// controller breaks the tie by the lower ICR index, and the record says so
// rather than implying the manual chose it. Plan BRD-3's rule that a pair the
// manual fixes is pinned by the test is satisfied: the manual fixes every pair
// this firmware can present, and the ambiguous pair cannot be created for a
// single external level because no two external pins share a level under any
// IRQPAR setting.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). The default build is
// Release and it defines NDEBUG. An out-of-range index is clamped to the
// valid domain rather than read through, so no index here can leave the table
// -- plan BRD-3's "the index domain is walked end to end" is satisfied by
// never indexing out of range at all.

#include "interruptController.h"

namespace g2
{
	namespace
	{
		// The within-level rank of an internal source, from the order Table 8-3
		// fixes: IP=11 highest (0), then IP=10 (1), then IP=01 (3), then IP=00
		// (4). The external pin at that level occupies rank 2, between IP=10
		// and IP=01.
		int internalRank(const uint8_t _icr)
		{
			const int ip = _icr & 0x03u;
			switch(ip)
			{
				case 3: return 0;  // IP=11
				case 2: return 1;  // IP=10
				case 1: return 3;  // IP=01
				default: return 4; // IP=00
			}
		}

		// The interrupt level IRQPAR assigns an external pin. IRQ7 is fixed at
		// 7; IRQ5 follows IRQPAR[2], IRQ3 follows IRQPAR[1], IRQ1 follows
		// IRQPAR[0]. MCF5307 UM Table 8-4.
		int externalLevel(const ExternalPin _pin, const uint8_t _irqpar)
		{
			switch(_pin)
			{
				case ExternalPin::Irq7: return 7;
				case ExternalPin::Irq5: return (_irqpar & 0x04u) ? 4 : 5;
				case ExternalPin::Irq3: return (_irqpar & 0x02u) ? 6 : 3;
				case ExternalPin::Irq1: return (_irqpar & 0x01u) ? 2 : 1;
			}
			return 0;
		}
	}

	InterruptController::InterruptController(void* _user, InterruptPresentFn _present)
		: m_user(_user)
		, m_present(_present)
	{
	}

	void InterruptController::writeRegister(const uint32_t _offset, const uint8_t _value)
	{
		if(_offset == gIrqparOffset)
			m_irqpar = _value;
		else if(_offset == gAvrOffset)
			m_avr = _value;
		else if(_offset >= gIcrBase && _offset < gIcrBase + gIcrCount)
			m_icr[_offset - gIcrBase] = _value;
		else
			return; // an offset this controller does not model changes nothing

		recomputeAndPresent();
	}

	uint8_t InterruptController::readRegister(const uint32_t _offset) const
	{
		if(_offset == gIrqparOffset)
			return m_irqpar;
		if(_offset == gAvrOffset)
			return m_avr;
		if(_offset >= gIcrBase && _offset < gIcrBase + gIcrCount)
			return m_icr[_offset - gIcrBase];
		return 0x00u;
	}

	void InterruptController::setInternalPending(const int _index, const bool _asserted)
	{
		if(_index < 0 || _index >= gInternalSourceCount)
			return;
		m_internalPending[_index] = _asserted;
		recomputeAndPresent();
	}

	void InterruptController::setExternalPending(const ExternalPin _pin, const bool _asserted)
	{
		const int index = static_cast<int>(_pin);
		if(index < 0 || index > 3)
			return;
		m_externalPending[index] = _asserted;
		recomputeAndPresent();
	}

	void InterruptController::setInternalVector(const int _index, const uint8_t _vector)
	{
		if(_index < 0 || _index >= gInternalSourceCount)
			return;
		m_internalVector[_index] = _vector;
		recomputeAndPresent();
	}

	void InterruptController::setExternalVector(const ExternalPin _pin, const uint8_t _vector)
	{
		const int index = static_cast<int>(_pin);
		if(index < 0 || index > 3)
			return;
		m_externalVector[index] = _vector;
		recomputeAndPresent();
	}

	InterruptController::Winner InterruptController::arbitrate() const
	{
		// Tier 1: the interrupt level, 7 highest to 1 lowest. Tier 2: the
		// within-level order Table 8-3 fixes. A source is a candidate only
		// when it is pending AND carries a programmed level 1..7; a level of 0
		// is an unassigned source and presents nothing.

		Winner winner;
		int bestLevel = 0;
		int bestRank = 5;   // the rank domain is 0..4; 5 is "no contender"
		int bestOrder = 0;  // deterministic tie-break among identical (level, rank)

		for(int i = 0; i < gInternalSourceCount; ++i)
		{
			if(!m_internalPending[i])
				continue;

			const uint8_t icr = m_icr[i];
			const int level = (icr >> 2) & 0x07u;
			if(level == 0)
				continue;

			const int rank = internalRank(icr);

			if(level > bestLevel
				|| (level == bestLevel && rank < bestRank)
				|| (level == bestLevel && rank == bestRank && i < bestOrder))
			{
				bestLevel = level;
				bestRank = rank;
				bestOrder = i;
				winner.valid = true;
				winner.level = level;
				winner.vector = m_internalVector[i];
				winner.autovector = (icr >> 7) & 0x01u;
			}
		}

		// The external pins. The external pin at a level occupies rank 2, the
		// position between the internal IP=10 source and the internal IP=01
		// source. No two external pins share a level under any IRQPAR setting,
		// so at most one external pin can contend at the winning level. The
		// order value is offset above the internal domain so it never
		// collides with an internal source's index; the tie-break path is
		// still there for symmetry and is unreachable for a single level.
		for(int p = 0; p < 4; ++p)
		{
			if(!m_externalPending[p])
				continue;

			const ExternalPin pin = static_cast<ExternalPin>(p);
			const int level = externalLevel(pin, m_irqpar);
			if(level == 0)
				continue;

			if(level > bestLevel
				|| (level == bestLevel && 2 < bestRank)
				|| (level == bestLevel && 2 == bestRank && p + gInternalSourceCount < bestOrder))
			{
				bestLevel = level;
				bestRank = 2;
				bestOrder = p + gInternalSourceCount;
				winner.valid = true;
				winner.level = level;
				winner.vector = m_externalVector[p];
				winner.autovector = (m_avr >> level) & 0x01u;
			}
		}

		return winner;
	}

	void InterruptController::recomputeAndPresent()
	{
		// No pending source presents MCF5307_IRQ_NONE (0). Design section
		// 5.2.2: the board presents its whole current state on every change,
		// and the call is idempotent, so presenting unconditionally is correct.
		const Winner winner = arbitrate();
		const int level = winner.valid ? winner.level : 0;
		const uint8_t vector = winner.valid ? winner.vector : 0u;
		const int autovector = winner.valid ? winner.autovector : 0;

		m_lastLevel = level;
		m_lastVector = vector;
		m_lastAutovector = autovector;

		if(m_present != nullptr)
			m_present(m_user, level, vector, autovector);
	}
}
