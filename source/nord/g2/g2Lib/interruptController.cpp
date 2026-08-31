// The two-tier interrupt controller.
//
// Facts from the MCF5307 User's Manual, section 8 "System Integration Module":
//
//   * Every interrupt source has its own ICR.
//   * The internal control registers sit at MBAR+$04C..$057, one per module
//     (Table 8-2). ICR10 and ICR11 at $056 and $057 are Reserved and generate
//     no source; the model carries the twelve storage slots and connects only
//     the first ten to sources.
//   * ICR layout: bit 7 AVEC, IL[2:0] at bits 4:2, IP[1:0] at bits 1:0. AVEC=0
//     means the source returns a vector during the interrupt-acknowledge
//     cycle; AVEC=1 means the SIM generates the autovector.
//   * The AVR at MBAR+$048, AVEC[7:1], is a bitmask over levels that
//     autovectors the external pin at each level.
//   * IRQPAR at MBAR+$006 (Table 8-4): IRQ5 is level 5 (IRQPAR[2]=0) or 4
//     (IRQPAR[2]=1); IRQ3 is level 3 (IRQPAR[1]=0) or 6 (IRQPAR[1]=1); IRQ1 is
//     level 1 (IRQPAR[0]=0) or 2 (IRQPAR[0]=1); IRQ7 is always level 7.
//   * The two-tier priority order (Table 8-3). Tier 1 is the interrupt level,
//     level 7 highest and level 1 lowest. Tier 2 is the within-level order the
//     table fixes: an internal source with IP=11 ranks highest, then IP=10,
//     then the external pin at that level, then IP=01, then IP=00.
//   * Only four external interrupt pins exist -- IRQ7, IRQ5, IRQ3 and IRQ1.
//
// The one explicit tie-untie in the arbitration. The manual fixes the order of
// every pair this controller can present except two internal sources pending
// at the same level and the same IP[1:0] -- the pair Table 8-3's footnote
// tells the programmer not to create. For that pair alone the controller
// breaks the tie by the lower ICR index; the manual did not choose it. The
// ambiguous pair cannot be created for a single external level because no two
// external pins share a level under any IRQPAR setting.
//
// An out-of-range index is clamped to the valid domain rather than read
// through, so no index here can leave the table.
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
		// IRQPAR[0]. MCF5307 um table 8-4.
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
		// No pending source presents MCF5307_IRQ_NONE (0). The board presents
		// its whole current state on every change and the call is idempotent,
		// so presenting unconditionally is correct.
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
