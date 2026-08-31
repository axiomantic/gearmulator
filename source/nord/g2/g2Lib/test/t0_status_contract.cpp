/* t0_status_contract.cpp -- the contract of g2::Status.
 *
 * A translation unit that includes status.h and compiles proves the file is on
 * disk and proves nothing about the type in it. The four properties below are
 * the contract: the scoping, the zero value, the roster and the
 * distinguishable failures.
 *
 * No case here is a language assert(). The default build type is Release and
 * Release defines NDEBUG, so an assert() would compile away and this check
 * would pass having checked nothing. Every run-time case reports through the
 * counter below; every compile-time case is a static_assert, which fires in
 * every build type.
 */

#include "status.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace
{
	int failures = 0;
	int cases = 0;

	void check(const bool condition, const char* const what)
	{
		++cases;

		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	/* Every value the type declares except the Count terminator, in declaration
	 * order. Held against Status::Count below so that an enumerator added to
	 * the header and forgotten here goes red rather than unnoticed. */
	constexpr g2::Status kRoster[] = {
		g2::Status::Unset,
		g2::Status::Ok,
		g2::Status::BadDspCount,
		g2::Status::BadFramesPerQuantum,
		g2::Status::BadBackend,
		g2::Status::BadHopFrames,
		g2::Status::BadRational,
		g2::Status::BadLookahead,
		g2::Status::BadDivider,
		g2::Status::BadMaxHostBlock,
		g2::Status::BridgesAttached,
	};

	constexpr size_t kRosterLength = sizeof(kRoster) / sizeof(kRoster[0]);
}

/* ------------- Property 1: a scoped enumeration over a fixed underlying type.
 *
 * These are the cases a running test cannot make. A failure here is a BUILD
 * failure and is reported as one. */

static_assert(std::is_enum_v<g2::Status>, "g2::Status is an enumeration");

static_assert(!std::is_convertible_v<g2::Status, int>,
	"g2::Status is SCOPED: it does not convert to int, so a caller cannot write if(st)");

static_assert(std::is_same_v<std::underlying_type_t<g2::Status>, uint32_t>,
	"g2::Status has a fixed underlying type of uint32_t");

int main()
{
	/* ------------- Property 2: Ok is not the zero value. */

	const g2::Status valueInitialised{};

	check(valueInitialised == g2::Status::Unset,
		"a value-initialised Status is Unset");
	check(valueInitialised != g2::Status::Ok,
		"a value-initialised Status is NOT Ok - a status nobody wrote must not read as success");
	check(g2::Status::Ok != g2::Status::Unset,
		"Ok and Unset are two different values");

	/* ------------- Property 3: contiguous from Unset, terminated by Count. */

	check(kRosterLength == static_cast<size_t>(g2::Status::Count),
		"the roster holds exactly as many values as Status::Count counts");

	for(size_t i = 0; i < kRosterLength; ++i)
	{
		char what[128];
		snprintf(what, sizeof(what), "roster entry %zu carries the value %zu", i, i);
		check(kRoster[i] == static_cast<g2::Status>(i), what);
	}

	/* ------------- Property 4: two distinguishable failures. */

	check(g2::Status::BadDspCount != g2::Status::BadDivider,
		"BadDspCount and BadDivider are distinguishable from each other");
	check(g2::Status::BadDspCount != g2::Status::Ok,
		"BadDspCount is distinguishable from Ok");
	check(g2::Status::BadDivider != g2::Status::Ok,
		"BadDivider is distinguishable from Ok");

	if(failures != 0)
	{
		printf("t0_status_contract: %d failure(s) in %d case(s)\n", failures, cases);
		return 1;
	}

	printf("t0_status_contract: all %d cases passed\n", cases);
	return 0;
}
