#include "interruptController.h"

namespace g2
{
	InterruptController::InterruptController() noexcept
	{
		reset();
	}

	void InterruptController::reset() noexcept
	{
		for (auto& src : m_sources)
		{
			src.pending = false;
			src.masked = true; // Default all masked
			src.level = 0;
			src.priorityWithinLevel = 0;
			src.vector = 0;
			src.autovector = false;
		}
	}

	void InterruptController::setSourceConfig(size_t _sourceId, int _level, int _priorityWithinLevel, uint8_t _vector, bool _autovector) noexcept
	{
		if (_sourceId >= kMaxSources)
			return;
		m_sources[_sourceId].level = _level;
		m_sources[_sourceId].priorityWithinLevel = _priorityWithinLevel;
		m_sources[_sourceId].vector = _vector;
		m_sources[_sourceId].autovector = _autovector;
	}

	const InterruptController::InterruptSource& InterruptController::getSourceConfig(size_t _sourceId) const noexcept
	{
		static const InterruptSource kDefaultSrc{};
		if (_sourceId >= kMaxSources)
			return kDefaultSrc;
		return m_sources[_sourceId];
	}

	void InterruptController::setPending(size_t _sourceId, bool _pending) noexcept
	{
		if (_sourceId < kMaxSources)
		{
			m_sources[_sourceId].pending = _pending;
		}
	}

	bool InterruptController::isPending(size_t _sourceId) const noexcept
	{
		if (_sourceId >= kMaxSources)
			return false;
		return m_sources[_sourceId].pending;
	}

	void InterruptController::setMasked(size_t _sourceId, bool _masked) noexcept
	{
		if (_sourceId < kMaxSources)
		{
			m_sources[_sourceId].masked = _masked;
		}
	}

	bool InterruptController::isMasked(size_t _sourceId) const noexcept
	{
		if (_sourceId >= kMaxSources)
			return true;
		return m_sources[_sourceId].masked;
	}

	uint32_t InterruptController::getImr() const noexcept
	{
		uint32_t imr = 0;
		for (size_t i = 0; i < kMaxSources; ++i)
		{
			if (m_sources[i].masked)
			{
				imr |= (1u << i);
			}
		}
		return imr;
	}

	void InterruptController::setImr(uint32_t _imr) noexcept
	{
		for (size_t i = 0; i < kMaxSources; ++i)
		{
			m_sources[i].masked = ((_imr & (1u << i)) != 0);
		}
	}

	uint32_t InterruptController::getIpr() const noexcept
	{
		uint32_t ipr = 0;
		for (size_t i = 0; i < kMaxSources; ++i)
		{
			if (m_sources[i].pending)
			{
				ipr |= (1u << i);
			}
		}
		return ipr;
	}

	InterruptController::EvaluationResult InterruptController::evaluate() const noexcept
	{
		EvaluationResult best{};
		best.level = MCF5307_IRQ_NONE;
		best.vector = 0;
		best.autovector = false;
		best.winningSourceId = -1;

		int bestLevel = 0;
		int bestSubPriority = -1;

		for (size_t i = 0; i < kMaxSources; ++i)
		{
			const auto& src = m_sources[i];
			if (!src.pending || src.masked || src.level <= 0)
			{
				continue;
			}

			// Tier 1: Higher IPL wins
			if (src.level > bestLevel)
			{
				bestLevel = src.level;
				bestSubPriority = src.priorityWithinLevel;
				best.level = src.level;
				best.vector = src.vector;
				best.autovector = src.autovector;
				best.winningSourceId = static_cast<int>(i);
			}
			// Tier 2: Equal IPL -> Higher priority within level wins
			else if (src.level == bestLevel && src.priorityWithinLevel > bestSubPriority)
			{
				bestSubPriority = src.priorityWithinLevel;
				best.level = src.level;
				best.vector = src.vector;
				best.autovector = src.autovector;
				best.winningSourceId = static_cast<int>(i);
			}
			// Tie-breaker: Equal IPL and sub-priority -> lower index wins (iteration 0..N-1 keeps first)
		}

		return best;
	}

	void InterruptController::updateCore(mcf5307_ctx* _ctx) const noexcept
	{
		if (!_ctx)
			return;

		EvaluationResult res = evaluate();
		mcf5307_set_irq(_ctx, res.level, res.vector, res.autovector ? 1 : 0);
	}

	void InterruptController::handleIack(int _level, uint8_t _vector) noexcept
	{
		for (size_t i = 0; i < kMaxSources; ++i)
		{
			auto& src = m_sources[i];
			if (src.pending && !src.masked && src.level == _level && (src.autovector || src.vector == _vector))
			{
				// Clear edge-triggered pending interrupt upon iack
				src.pending = false;
				break;
			}
		}
	}
}
