#pragma once

#include <cstddef>
#include <cstdint>

#include "dspContext.h"

namespace rg2
{
	inline constexpr size_t kJobCount = 8;

	class Executor
	{
	public:
		virtual ~Executor() = default;

		using JobFn = void (*)(JobContext* ctx) noexcept;

		struct Job
		{
			JobFn fn;
			JobContext* ctx;
		};

		// Runs every job exactly once. Non-reentrant.
		virtual void run(const Job* jobs, size_t count) noexcept = 0;
		virtual bool isSerial() const noexcept = 0;
	};

	// Runs jobs in order on the calling thread.
	class SerialExecutor final : public Executor
	{
	public:
		void run(const Job* jobs, size_t count) noexcept override;
		bool isSerial() const noexcept override;

		// 0 outside run(), 1 inside a dispatched job.
		uint32_t depth() const noexcept;

		// Count of refused re-entrant calls to run().
		uint64_t reentryCount() const noexcept;

	private:
		uint32_t m_depth = 0;
		uint64_t m_reentries = 0;
	};
} // namespace rg2
