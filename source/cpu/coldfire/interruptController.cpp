// The two-tier interrupt controller.

#include "interruptController.h"

namespace coldfire
{
	namespace
	{
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
