/* The check asserts the full member list, and not the layout alone.
 *
 * Asserting only the JobFault enumerators, that `base` is first,
 * is_standard_layout_v and offsetof(DspContext, base) == 0 would be satisfied
 * by
 *
 *     struct DspContext { JobContext base; };
 *
 * against which dspJob cannot be written at all. So this check asserts each
 * member declared beside `base` by name and by type, with a static_assert on
 * each.
 *
 * Four of them exist because the scheduler drives the ESAI frame, and each
 * carries its own named assertion: audioEsai, secondEsai, frameIndex and
 * secondBusFrameDivider. FrameIndex is the one the Scheduler writes before
 * Executor::run and no job writes. Deleting frameIndex from DspContext makes
 * this check fail.
 *
 * The member list is a compile-time property, so a deleted or renamed member is
 * a build failure rather than a check report. The runtime half below is what
 * this registered program reports on: the pointer recovery the executor
 * performs, and that the members are distinct objects rather than names that
 * alias one another.
 */

#include "dspContext.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
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
 * Each value by name. CoreHalted is the Scheduler's mapping of
 * Board::faulted(). It exists so that contextFault(i) != None is a valid fault
 * test for every context index and not for the DSPs only. */

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
 * It holds one field. The size assertion is the mechanical form of that
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
 * Standard layout is load-bearing, not incidental. Job::ctx is a JobContext*
 * and the job body must recover the DspContext from it. That recovery is legal
 * Only because DspContext is a standard-layout type, which makes it
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
	"DspContext::position -- the chain position.");
static_assert(std::is_same_v<decltype(g2::DspContext::rate), Rational>,
	"DspContext::rate -- cycles for each frame.");
static_assert(std::is_same_v<decltype(g2::DspContext::acc), uint32_t>,
	"DspContext::acc -- the rational accumulator.");
static_assert(std::is_same_v<decltype(g2::DspContext::debt), int64_t>,
	"DspContext::debt -- the cycle debt. It is SIGNED, "
	"because the want and debt block computes a signed difference.");
static_assert(std::is_same_v<decltype(g2::DspContext::longDispatchQuanta),
		uint64_t>,
	"DspContext::longDispatchQuanta -- the long-dispatch counter.");
static_assert(std::is_same_v<decltype(g2::DspContext::dsp), dsp56k::DSP*>,
	"DspContext::dsp -- borrowed. The Scheduler owns the DSP set.");

/* The four the scheduler needs BECAUSE it DRIVES the ESAI FRAME. Without them
 * the job body cannot name the port it must advance, and it cannot decide
 * whether this quantum is inside the second bus's advance window. */

static_assert(std::is_same_v<decltype(g2::DspContext::audioEsai),
		dsp56k::Esai*>,
	"DspContext::audioEsai -- borrowed, the X-space ESAI.");
static_assert(std::is_same_v<decltype(g2::DspContext::secondEsai),
		dsp56k::Esai*>,
	"DspContext::secondEsai -- borrowed, the Y-space ESAI_1.");
static_assert(std::is_same_v<decltype(g2::DspContext::frameIndex), uint64_t>,
	"DspContext::frameIndex -- the virtual frame index for the quantum ABOUT "
	"TO RUN. The Scheduler writes it into every DspContext before it calls "
	"Executor::run, and NO job writes it.");
static_assert(std::is_same_v<decltype(g2::DspContext::secondBusFrameDivider),
		unsigned>,
	"DspContext::secondBusFrameDivider -- the second-bus frame divider D, from "
	"G2_SECOND_BUS_FRAME_DIVIDER. The job body advances the second bus only "
	"when frameIndex % secondBusFrameDivider == 0.");

/* THE GATE's own member. t0_dsp_run_gate pins the same type because the gate is
 * the run gate's, and it is repeated here because this file is the one that claims to
 * name every member. */
static_assert(std::is_same_v<decltype(g2::DspContext::programLanded),
		const bool*>,
	"DspContext::programLanded -- borrowed, and NULL means NOT landed.");

/* The default direction is structural and not conventional. programLanded
 * carries a default member initializer, which is what makes `g2::DspContext c;`
 * a closed gate rather than an indeterminate one. A member with an initializer
 * gives the class a non-trivial default constructor, so this assertion is what
 * a deleted initializer runs into. */
static_assert(!std::is_trivially_default_constructible_v<g2::DspContext>,
	"DspContext must carry a default member initializer, so a context declared "
	"without braces reads NOT landed rather than indeterminate.");

/* No EsaiClock ANYWHERE. An EsaiClock cannot follow a rational
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

	/* ---------------- a context declared WITHOUT braces closes the gate.
	 *
	 * The storage is poisoned first, so the read below is a value the
	 * initializer wrote and not one the stack happened to hold. Every other
	 * member stays 0xFF and none is read. */
	{
		alignas(g2::DspContext) unsigned char storage[sizeof(g2::DspContext)];
		std::memset(storage, 0xFF, sizeof storage);

		auto* const fresh = new (storage) g2::DspContext;

		check(fresh->programLanded == nullptr,
			"a context declared without braces reads NOT landed");

		fresh->~DspContext();
	}

	/* ---------------- a zeroed context carries no fault.
	 *
	 * JobFault::None is zero, so the Scheduler's own reset can clear a whole
	 * context and the fault field then reads None. The field is sticky: a job
	 * never clears it and only Scheduler::reset does. What this check owns is
	 * that None is the zero value that rule needs. */
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
