/* t0_dsp_context_layout.cpp -- the check of task SCH-6. Design 13.10.3.
 *
 * THE CHECK ASSERTS THE FULL MEMBER LIST, AND NOT THE LAYOUT ALONE.
 *
 * A check of the JobFault enumerators, that JobContext holds one field, that
 * `base` is first, is_standard_layout_v and offsetof(DspContext, base) == 0 is
 * not enough. A struct declared as
 *
 *     struct DspContext { JobContext base; };
 *
 * PASSES EVERY ONE OF THOSE, and dspJob cannot be written at all against that
 * struct. So this check asserts the members design section 13.10.3 declares
 * beside `base`, BY NAME AND BY TYPE, with a static_assert on each.
 *
 * A check that a bare one-member struct satisfies is not a weak check; it is a
 * check that cannot fail.
 *
 * FOUR OF THE TEN EXIST BECAUSE THE SCHEDULER DRIVES THE ESAI FRAME, and each
 * carries its own named assertion: audioEsai, secondEsai, frameIndex and
 * secondBusFrameDivider. frameIndex is the one SCH-19 writes before
 * Executor::run and no job writes. DELETING frameIndex FROM DspContext MAKES
 * THIS CHECK FAIL, and that is the acceptance criterion.
 *
 * WHERE EACH HALF FAILS. The member list is a COMPILE-TIME property, so a
 * deleted or renamed member is a compile error and plan section 7.7.1 classes
 * that as a build failure rather than a check report. The runtime half below
 * is what this registered program reports on: the pointer recovery the
 * executor performs, and that the ten members are ten distinct objects rather
 * than names that alias one another.
 */

#include "dspContext.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}
}

/* ================ JobFault
 *
 * The five values, each by name. CoreHalted is the fifth and it is the
 * Scheduler's mapping of Board::faulted(). It exists so that
 * contextFault(i) != None is a valid fault test for EVERY context index and
 * not for the DSPs only; without it the MCU row of that test is a false
 * negative. */

static_assert(std::is_enum_v<g2::JobFault>,
	"JobFault is an enumeration.");
static_assert(std::is_same_v<std::underlying_type_t<g2::JobFault>, uint32_t>,
	"JobFault's underlying type is uint32_t.");
static_assert(!std::is_convertible_v<g2::JobFault, uint32_t>,
	"JobFault is a SCOPED enumeration, so no fault value converts to an "
	"integer by accident.");

static_assert(static_cast<uint32_t>(g2::JobFault::None) == 0u,
	"JobFault::None is zero, so a zeroed context carries no fault.");
static_assert(static_cast<uint32_t>(g2::JobFault::IllegalInstruction) == 1u,
	"JobFault::IllegalInstruction.");
static_assert(static_cast<uint32_t>(g2::JobFault::MemoryFault) == 2u,
	"JobFault::MemoryFault.");
static_assert(static_cast<uint32_t>(g2::JobFault::BackendFault) == 3u,
	"JobFault::BackendFault.");
static_assert(static_cast<uint32_t>(g2::JobFault::CoreHalted) == 4u,
	"JobFault::CoreHalted. THE MCU CONTEXT ONLY.");

static_assert(g2::JobFault::CoreHalted != g2::JobFault::None,
	"CoreHalted is a fault, so contextFault(i) != None is a valid test for "
	"the MCU index too.");

/* ================ JobContext
 *
 * It holds ONE field. The size assertion is the mechanical form of that
 * sentence: a second field of any type would make the struct larger than the
 * one field it may carry. */

static_assert(std::is_standard_layout_v<g2::JobContext>,
	"JobContext is standard-layout, because DspContext's own standard layout "
	"depends on every member being one.");
static_assert(std::is_same_v<decltype(g2::JobContext::fault), g2::JobFault>,
	"JobContext::fault is a JobFault.");
static_assert(sizeof(g2::JobContext) == sizeof(g2::JobFault),
	"JobContext holds ONE field. A second field would make it larger.");
static_assert(offsetof(g2::JobContext, fault) == 0,
	"JobContext::fault is the whole of JobContext.");

/* ================ DspContext -- the layout
 *
 * STANDARD LAYOUT IS LOAD-BEARING, NOT INCIDENTAL. Job::ctx is a JobContext*
 * and the job body must recover the DspContext from it. That recovery is legal
 * ONLY because DspContext is a standard-layout type, which makes it
 * pointer-interconvertible with its first member. A later member that broke
 * the property would make every job body undefined behaviour with no
 * diagnostic. */

static_assert(std::is_standard_layout_v<g2::DspContext>,
	"DspContext is standard-layout, which is what makes the executor's "
	"pointer recovery legal.");
static_assert(offsetof(g2::DspContext, base) == 0,
	"JobContext base MUST be the first member. Job::ctx points at it.");
static_assert(std::is_same_v<decltype(g2::DspContext::base), g2::JobContext>,
	"DspContext::base is a JobContext.");

/* ================ DspContext -- the ten members beside `base`
 *
 * Each one by name and by type. This is the block a bare one-member struct
 * fails. */

static_assert(std::is_same_v<decltype(g2::DspContext::position), unsigned>,
	"DspContext::position -- the chain position of design section 12.3.");
static_assert(std::is_same_v<decltype(g2::DspContext::rate), Rational>,
	"DspContext::rate -- cycles for each frame, design section 13.4.1.");
static_assert(std::is_same_v<decltype(g2::DspContext::acc), uint32_t>,
	"DspContext::acc -- the rational accumulator, design section 13.4.1.");
static_assert(std::is_same_v<decltype(g2::DspContext::debt), int64_t>,
	"DspContext::debt -- the cycle debt, design section 13.4.6. It is SIGNED, "
	"because the want and debt block computes a signed difference.");
static_assert(std::is_same_v<decltype(g2::DspContext::longDispatchQuanta),
		uint64_t>,
	"DspContext::longDispatchQuanta -- the rule 4 counter, design section "
	"13.4.6.");
static_assert(std::is_same_v<decltype(g2::DspContext::dsp), dsp56k::DSP*>,
	"DspContext::dsp -- borrowed. The Scheduler owns the DSP set.");

/* The four the scheduler needs BECAUSE IT DRIVES THE ESAI FRAME. Without them
 * the job body cannot name the port it must advance, and it cannot decide
 * whether this quantum is inside the second bus's advance window. */

static_assert(std::is_same_v<decltype(g2::DspContext::audioEsai),
		dsp56k::Esai*>,
	"DspContext::audioEsai -- borrowed, the X-space ESAI of design section "
	"11.1.");
static_assert(std::is_same_v<decltype(g2::DspContext::secondEsai),
		dsp56k::Esai*>,
	"DspContext::secondEsai -- borrowed, the Y-space ESAI_1 of design section "
	"11.1.");
static_assert(std::is_same_v<decltype(g2::DspContext::frameIndex), uint64_t>,
	"DspContext::frameIndex -- the virtual frame index for the quantum ABOUT "
	"TO RUN. The Scheduler writes it into every DspContext before it calls "
	"Executor::run, and NO job writes it.");
static_assert(std::is_same_v<decltype(g2::DspContext::secondBusFrameDivider),
		unsigned>,
	"DspContext::secondBusFrameDivider -- design section 12.3's D, from "
	"G2_SECOND_BUS_FRAME_DIVIDER. The job body advances the second bus only "
	"when frameIndex % secondBusFrameDivider == 0.");

/* NO EsaiClock ANYWHERE. An EsaiClock cannot follow a rational
 * cycles-for-each-frame rate, so the scheduler drives the frame instead and
 * this context carries the two ports rather than a clock. The type is not even
 * named here, and it is named nowhere in g2Lib. */

int main()
{
	/* ---------------- the pointer recovery the executor performs.
	 *
	 * Job::ctx is a JobContext* that points at &context.base, and the job body
	 * recovers the DspContext with reinterpret_cast on the pointer and nothing
	 * else. */
	{
		g2::DspContext context{};

		g2::JobContext* const asJobContext = &context.base;

		check(static_cast<void*>(asJobContext) == static_cast<void*>(&context),
			"a DspContext and its base share one address, which is what makes "
			"the recovery legal");

		auto* const recovered = reinterpret_cast<g2::DspContext*>(asJobContext);

		check(recovered == &context,
			"the executor's recovery idiom returns the original context");
	}

	/* ---------------- the ten members are ten distinct objects.
	 *
	 * A distinct value is written into each and every one is read back. Two
	 * members that shared storage -- a union, or one name declared twice --
	 * would satisfy every type assertion above and would fail here. */
	{
		dsp56k::DSP*  const dspAddress    = reinterpret_cast<dsp56k::DSP*>(
			static_cast<uintptr_t>(0x1000u));
		dsp56k::Esai* const audioAddress  = reinterpret_cast<dsp56k::Esai*>(
			static_cast<uintptr_t>(0x2000u));
		dsp56k::Esai* const secondAddress = reinterpret_cast<dsp56k::Esai*>(
			static_cast<uintptr_t>(0x3000u));

		g2::DspContext context{};

		context.base.fault            = g2::JobFault::BackendFault;
		context.position              = 5u;
		context.rate                  = Rational{ 150u, 96u };
		context.acc                   = 7u;
		context.debt                  = -11;
		context.longDispatchQuanta    = 13u;
		context.dsp                   = dspAddress;
		context.audioEsai             = audioAddress;
		context.secondEsai            = secondAddress;
		context.frameIndex            = 17u;
		context.secondBusFrameDivider = 19u;

		check(context.base.fault == g2::JobFault::BackendFault,
			"base.fault holds what was written into it");
		check(context.position == 5u, "position holds its own value");
		check(context.rate.num == 150u && context.rate.den == 96u,
			"rate holds its own value");
		check(context.acc == 7u, "acc holds its own value");
		check(context.debt == -11,
			"debt holds its own value, and it holds a NEGATIVE one");
		check(context.longDispatchQuanta == 13u,
			"longDispatchQuanta holds its own value");
		check(context.dsp == dspAddress, "dsp holds its own value");
		check(context.audioEsai == audioAddress,
			"audioEsai holds its own value");
		check(context.secondEsai == secondAddress,
			"secondEsai holds its own value");
		check(context.frameIndex == 17u, "frameIndex holds its own value");
		check(context.secondBusFrameDivider == 19u,
			"secondBusFrameDivider holds its own value");

		/* The two ESAI pointers are separate members and not one member read
		 * twice, which is the specific confusion the second bus invites. */
		check(context.audioEsai != context.secondEsai,
			"audioEsai and secondEsai are two members, not one");
	}

	/* ---------------- a zeroed context carries no fault.
	 *
	 * JobFault::None is zero, so the Scheduler's own reset can clear a whole
	 * context and the fault field then reads None. The field is STICKY --
	 * a job never clears it and only Scheduler::reset does -- and that rule
	 * belongs to the Scheduler, which SCH-19 and SCH-20 carry. What this task
	 * owns is that None is the zero value the rule needs. */
	{
		const g2::DspContext zeroed{};

		check(zeroed.base.fault == g2::JobFault::None,
			"a zeroed context carries JobFault::None");
		check(zeroed.frameIndex == 0u,
			"a zeroed context starts at frame index zero");
	}

	if(failures != 0)
	{
		printf("t0_dsp_context_layout: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_dsp_context_layout: all cases passed\n");
	return 0;
}
