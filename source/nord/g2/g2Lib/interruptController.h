#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include "mcf5307.h"

namespace g2
{
	/* Clean-room implementation of two-tier priority interrupt controller
	 * per MCF5307 User's Manual §8 (SIM Interrupt Controller).
	 *
	 * Two-tier priority scheme:
	 * - Tier 1: Interrupt Priority Level (IPL), levels 1 through 7 (7 = NMI, 0 = disabled/none).
	 * - Tier 2: Sub-priority within level (0 through 3, where 3 is highest priority).
	 * - Tie-breaker: Lowest source index (0 to MaxSources-1).
	 *
	 * Interrupt Masking:
	 * - Interrupt Mask Register (IMR): bit set = source masked (disabled).
	 * - Interrupt Pending Register (IPR): bit set = interrupt pending.
	 */
	class InterruptController
	{
	public:
		static constexpr size_t kMaxSources = 32;

		struct InterruptSource
		{
			bool pending{false};
			bool masked{true}; // true = masked (disabled in IMR)
			int level{0}; // 0 = disabled, 1..7 = IPL
			int priorityWithinLevel{0}; // 0..3 (3 = highest)
			uint8_t vector{0};
			bool autovector{false};
		};

		struct EvaluationResult
		{
			int level{MCF5307_IRQ_NONE}; // 0..7
			uint8_t vector{0};
			bool autovector{false};
			int winningSourceId{-1};
		};

		InterruptController() noexcept;

		void reset() noexcept;

		// Source configuration
		void setSourceConfig(size_t _sourceId, int _level, int _priorityWithinLevel, uint8_t _vector, bool _autovector = false) noexcept;
		const InterruptSource& getSourceConfig(size_t _sourceId) const noexcept;

		// Interrupt pending control
		void setPending(size_t _sourceId, bool _pending) noexcept;
		bool isPending(size_t _sourceId) const noexcept;

		// Interrupt masking (IMR)
		void setMasked(size_t _sourceId, bool _masked) noexcept;
		bool isMasked(size_t _sourceId) const noexcept;

		// Full IMR access (32-bit)
		uint32_t getImr() const noexcept;
		void setImr(uint32_t _imr) noexcept;

		// Full IPR access (32-bit)
		uint32_t getIpr() const noexcept;

		// Two-tier evaluation per MCF5307 UM §8
		EvaluationResult evaluate() const noexcept;

		// Core integration helper
		void updateCore(mcf5307_ctx* _ctx) const noexcept;

		// IACK handler callback
		void handleIack(int _level, uint8_t _vector) noexcept;

	private:
		std::array<InterruptSource, kMaxSources> m_sources{};
	};
}
