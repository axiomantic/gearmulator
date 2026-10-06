#pragma once

#include <cstdint>

#include "rg2/timebase.h"

namespace rg2
{
	// Advances one quantum under the cycle-debt rule for a context (DspContext or McuContext).
	// Returns emulated cycles spent this quantum (0 if want <= 0).
	template <typename Ctx, typename Run> int64_t runQuantum(Ctx& ctx, Run&& run) noexcept
	{
		const int64_t budget = static_cast<int64_t>(alloc(ctx.rate, &ctx.acc));
		const int64_t want = budget - ctx.debt;

		if (want <= 0)
		{
			// Previous quantum overran budget: pay debt down and count long dispatch.
			ctx.debt -= budget;
			++ctx.longDispatchQuanta;
			return 0;
		}

		const int64_t spent = static_cast<int64_t>(run(static_cast<uint32_t>(want)));

		ctx.debt = spent - want;
		if (ctx.debt < 0)
			ctx.debt = 0; // No credit banking: under-budget run does not accumulate credit.

		return spent;
	}
} // namespace rg2
