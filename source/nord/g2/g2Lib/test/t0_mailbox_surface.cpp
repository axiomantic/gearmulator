/* t0_mailbox_surface.cpp -- the check of task CHN-1. Design 12.3, 13.10.2.
 *
 * THE PROPERTIES THIS ROW OWNS, AND THAT A TARGET BUILD CANNOT SEE:
 *
 *  1. The ring holds EXACTLY hopFrames + 1 frames, asserted for hopFrames = 1
 *     and hopFrames = 2 against the object's OWN REPORTED depth (the depth()
 *     accessor), and cross-checked against the constructor's allocation byte
 *     count divided by sizeof(Frame).
 *
 *  2. The WHOLE ALLOCATION HAPPENS ONCE, IN THE CONSTRUCTOR, asserted against
 *     an allocation counter: the constructor performs exactly one allocation,
 *     and 1,000 advance() calls after it perform zero.
 *
 *  3. writeSlot(unsigned) is DECLARED RETURNING SlotWriteView, asserted with a
 *     static_assert on decltype (which also pins the rest of the Tier 1
 *     surface: read() returns const Frame&, write() returns Frame&, and both
 *     with advance() are noexcept as section 12.3 declares).
 *
 * THE ALLOCATION COUNTER IS A GLOBAL operator new/delete PAIR, armed only
 * around the specific constructor call and around the advance loop. Nothing
 * is allocated or printed inside an armed window, so a window's count is the
 * mailbox's own allocation count and nothing else.
 */

#include "mailbox.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

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

	void checkEqual(const uint64_t observed, const uint64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what,
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	/* The allocation counter. The `armed` flag is true only across a
	 * measured window; calls and bytes count allocations made inside that
	 * window. */
	struct AllocStats
	{
		bool      armed = false;
		uint64_t  calls = 0;
		uint64_t  bytes = 0;
	};

	AllocStats g_alloc;

	void armAlloc()
	{
		g_alloc = AllocStats{ true, 0u, 0u };
	}

	void disarmAlloc()
	{
		g_alloc.armed = false;
	}
}

/* The global operator new/delete pair that the counter observes. Both forms
 * of each operator are provided so that the compiler is not forced into the
 * nothrow or aligned replacements in an ABI-dependent way. */
void* operator new(std::size_t n)
{
	if(g_alloc.armed)
	{
		++g_alloc.calls;
		g_alloc.bytes += n;
	}
	if(void* const p = std::malloc(n))
		return p;
	throw std::bad_alloc();
}

void* operator new[](std::size_t n)
{
	if(g_alloc.armed)
	{
		++g_alloc.calls;
		g_alloc.bytes += n;
	}
	if(void* const p = std::malloc(n))
		return p;
	throw std::bad_alloc();
}

void operator delete(void* p) noexcept
{
	std::free(p);
}

void operator delete[](void* p) noexcept
{
	std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
	std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
	std::free(p);
}

/* THE COMPILE-TIME SURFACE. decltype of a call in an unevaluated context
 * does not call the function, so these pin the declared signatures and can
 * fail only at compile time -- exactly the property a target build cannot
 * see. */
static_assert(std::is_same_v<decltype(std::declval<const g2::Mailbox&>().read()),
	g2::Frame const&>,
	"read() returns const Frame&; a consuming DSP must not modify the "
	"frame it reads.");
static_assert(std::is_same_v<decltype(std::declval<g2::Mailbox&>().write()),
	g2::Frame&>,
	"write() returns Frame&; a producing DSP fills the frame it writes.");
static_assert(noexcept(std::declval<g2::Mailbox&>().read()),
	"read() is noexcept, as design section 12.3 declares.");
static_assert(noexcept(std::declval<g2::Mailbox&>().write()),
	"write() is noexcept, as design section 12.3 declares.");
static_assert(noexcept(std::declval<g2::Mailbox&>().advance()),
	"advance() is noexcept, as design section 12.3 declares.");
static_assert(std::is_same_v<decltype(std::declval<g2::Mailbox&>().writeSlot(0u)),
	g2::SlotWriteView>,
	"writeSlot(unsigned) is declared returning SlotWriteView.");
static_assert(noexcept(std::declval<g2::Mailbox&>().writeSlot(0u)),
	"writeSlot() is noexcept.");
static_assert(std::is_same_v<decltype(std::declval<const g2::Mailbox&>().depth()),
	unsigned>,
	"depth() reports the ring depth as an unsigned.");

/* Drives the two measurement windows for one hop delay and reports the three
 * properties. Returns true when the case passed. */
bool driveCase(const unsigned hopFrames, const char* const what)
{
	bool ok = true;

	/* ---------------- property 1 and 2: the constructor window. */
	armAlloc();
	{
		g2::Mailbox mailbox(hopFrames);

		if(mailbox.depth() != hopFrames + 1u)
		{
			printf("FAIL %s: reported depth %u, expected hopFrames + 1 = %u\n",
				what, mailbox.depth(), hopFrames + 1u);
			ok = false;
		}

		/* The whole ring is ONE allocation of exactly (hopFrames + 1) frames.
		 * This is the second reading of "the object's own reported depth":
		 * the bytes the object asked for in its constructor divided by the
		 * size of a frame must equal the depth, and it must be a whole
		 * number of frames. */
		if(g_alloc.calls != 1u)
		{
			printf("FAIL %s: the constructor made %llu allocations, not 1\n",
				what, static_cast<unsigned long long>(g_alloc.calls));
			ok = false;
		}
		checkEqual(g_alloc.bytes % sizeof(g2::Frame), 0u, what);

		const uint64_t derivedDepth = g_alloc.bytes / sizeof(g2::Frame);
		if(derivedDepth != static_cast<uint64_t>(hopFrames + 1u))
		{
			printf("FAIL %s: the allocation implies %llu frames, not %u\n",
				what, static_cast<unsigned long long>(derivedDepth),
				hopFrames + 1u);
			ok = false;
		}

		/* The mailbox is DESTROYED before the counter is disarmed, so the
		 * vector's deallocation is also observed and subtracted from
		 * nothing -- but the deallocation is not an allocation, and what
		 * matters below is that the run does not grow the ring. */
	}
	disarmAlloc();

	/* ---------------- property 2, the run window: 1,000 advances allocate
	 * nothing.
	 *
	 * The mailbox is constructed OUTSIDE the armed window, so the counter
	 * observes only the 1,000 advance() calls and any allocation they might
	 * make. A body that grew the ring on advance() would trip this as a
	 * call count above zero; the whole-allocation-once rule of the header is
	 * what makes the counter stay at zero. */
	{
		g2::Mailbox mailbox(hopFrames);
		armAlloc();
		for(unsigned i = 0u; i < 1000u; ++i)
			mailbox.advance();
		disarmAlloc();
	}
	if(g_alloc.calls != 0u)
	{
		printf("FAIL %s: 1,000 advance() calls made %llu allocations\n", what,
			static_cast<unsigned long long>(g_alloc.calls));
		ok = false;
	}

	return ok;
}

int main()
{
	/* The plan asserts the depth for hopFrames = 1 and hopFrames = 2. */
	bool ok = driveCase(1u, "hopFrames = 1");
	ok = driveCase(2u, "hopFrames = 2") && ok;

	if(!ok || failures != 0)
	{
		printf("t0_mailbox_surface: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_mailbox_surface: all cases passed\n");
	return 0;
}
