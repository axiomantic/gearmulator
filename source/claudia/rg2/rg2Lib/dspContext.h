#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "rg2/timebase.h"

namespace dsp56k
{
	class DSP;
	class Esai;
} // namespace dsp56k

namespace rg2
{
	// Fault conditions reported by execution contexts.
	enum class JobFault : uint32_t
	{
		None = 0,
		IllegalInstruction = 1,
		MemoryFault = 2,
		BackendFault = 3,
		CoreHalted = 4 // MCU context only
	};

	struct JobContext
	{
		JobFault fault;
	};

	// Standard layout: recovered from JobContext* via pointer interconversion.
	struct DspContext
	{
		JobContext base; // MUST be first.
		unsigned position; // 0 .. dspCount-1 (chain position)
		Rational rate; // Cycles per frame
		uint32_t acc; // Rational accumulator
		int64_t debt; // Cycle debt
		uint64_t longDispatchQuanta;
		dsp56k::DSP* dsp; // Borrowed DSP core

		uint32_t slotBudgetDivisor;
		uint32_t slotDispatches;

		dsp56k::Esai* audioEsai; // Borrowed X-space ESAI
		dsp56k::Esai* secondEsai; // Borrowed Y-space ESAI_1

		uint64_t frameIndex;
		const bool* programLanded = nullptr; // nullptr means program has not landed
		unsigned secondBusFrameDivider;
	};

	static_assert(std::is_standard_layout_v<DspContext>,
				  "DspContext must be standard-layout. The executor recovers it from a "
				  "pointer to its first member, and that is legal only for a "
				  "standard-layout type.");
	static_assert(offsetof(DspContext, base) == 0,
				  "JobContext base must be the FIRST member of DspContext. Job::ctx "
				  "points at it.");
} // namespace rg2
