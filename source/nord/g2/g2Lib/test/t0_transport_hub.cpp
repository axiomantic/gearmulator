/* t0_transport_hub.cpp -- the check of task SCH-29.
 * Design sections 13.10.6, 15.1 and 18.2.
 *
 * WHAT THIS ROW OWNS, AND WHY EACH CASE CANNOT BE REPLACED BY A BUILD:
 *
 *  1. THE FIXED ALLOCATION. The hub's whole allocation happens in the
 *     constructor and its byte total equals
 *         kMaxEndpoints x queueDepth x (sizeof(StampedFrame) + maxFrameBytes)
 *     NOT ONE FACTOR OF THAT PRODUCT IS WRITTEN AS A LITERAL HERE. The
 *     endpoint count is read from g2::TransportHub::kMaxEndpoints, the two
 *     sizes are this test's own constructor arguments, and the derived
 *     ceiling is computed from section 15.3's wire framing -- one type byte,
 *     a two-byte length and the largest payload a two-byte length can name.
 *     A literal 3, a literal 65538 or a literal 3146976 would be this
 *     project's signature defect: a constant standing where the generating
 *     count belongs.
 *
 *  2. NOTHING ALLOCATES AFTER CONSTRUCTION. This is the property that is
 *     easiest to assert vacuously, so it is asserted against an ARMED global
 *     operator new/delete pair, in the shape t0_mailbox_surface.cpp already
 *     uses, across a window that drives every declared method -- attach,
 *     toDevice at and past both refusal boundaries, drainToDevice, fromDevice
 *     and detach -- and the window's own byte and call totals must both be
 *     zero. The endpoints used inside the window carry FIXED ARRAYS and no
 *     container, so a non-zero count inside the window is the hub's and
 *     nothing else's.
 *
 *  3. THE DECLARATION ORDER. ProtocolFrame and StampedFrame are declared
 *     BEFORE TransportHub. This is a property of the header's source text and
 *     no C++ expression can read it, so the check reads the header itself --
 *     the path arrives on argv[1] rather than being guessed, for the reason
 *     t0_clock_guard and t0_block_table_harness already give. The search
 *     carries its own known-negative: a token that is not in the file must
 *     report absent, so a "not found" from this scanner is an absence and not
 *     an unread file.
 *
 *  4. THE TWO REFUSALS. A frame larger than maxFrameBytes is refused, and a
 *     frame offered to an endpoint whose queue already holds queueDepth
 *     frames is refused. Both report through the RETURNED BOOL.
 *
 *  5. THE FIXED ENDPOINT ORDER AND THE STAMP. drainToDevice visits endpoints
 *     in attachment order and stamps every frame it drains with the index of
 *     the quantum it drained in. Frames delivered from three endpoints in the
 *     same quantum reach the device in attachment order, on every quantum of
 *     a long run.
 *
 *  6. THE BORROW LIFETIME. The pointers one drainToDevice returns are still
 *     readable IMMEDIATELY BEFORE the next drainToDevice on the same hub, and
 *     this test reads them there -- after further toDevice traffic has run
 *     against the same endpoint, which is the case a naive ring would fail.
 *
 * NOTHING IN THIS FILE IS AN assert() AND NOTHING CATCHES AN EXCEPTION. The
 * default build type of this repository is Release and Release defines NDEBUG,
 * which deletes every assert(); every run-time verdict here reports through
 * this file's own failure counter, and every compile-time verdict is a
 * static_assert, which fires in every build type.
 */

#include "transportHub.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

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

	/* The allocation counter. `armed` is true only across a measured window;
	 * calls and bytes count allocations made inside that window. */
	struct AllocStats
	{
		bool     armed = false;
		uint64_t calls = 0;
		uint64_t bytes = 0;
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

/* The global operator new/delete pair the counter observes. Both forms of each
 * operator are provided so that the compiler is not forced into the nothrow or
 * aligned replacement in an ABI-dependent way. */
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

void operator delete(void* p) noexcept              { std::free(p); }
void operator delete[](void* p) noexcept            { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
	/* ---------------------------------------------------------------- the
	 * declared surface, pinned through fully qualified types. Taking the
	 * address of a member function odr-uses it, so a renamed or re-signed
	 * method is a COMPILE error here and a declared-but-undefined one is a
	 * LINK error. A build of the library alone sees neither. */
	void (g2::TransportHub::* const kAttach)(g2::TransportEndpoint&)
		= &g2::TransportHub::attach;
	void (g2::TransportHub::* const kDetach)(g2::TransportEndpoint&)
		= &g2::TransportHub::detach;
	void (g2::TransportHub::* const kFromDevice)(g2::ProtocolFrame) noexcept
		= &g2::TransportHub::fromDevice;
	bool (g2::TransportHub::* const kToDevice)(g2::TransportEndpoint&,
		g2::ProtocolFrame) noexcept = &g2::TransportHub::toDevice;
	size_t (g2::TransportHub::* const kDrainToDevice)(g2::StampedFrame*,
		size_t) noexcept = &g2::TransportHub::drainToDevice;

	static_assert(std::is_constructible_v<g2::TransportHub, size_t, size_t>,
		"TransportHub(size_t maxFrameBytes, size_t queueDepth) is the "
		"declared constructor: the hub cannot derive either size for itself, "
		"and both are needed before the first allocation.");

	static_assert(std::is_same_v<decltype(g2::ProtocolFrame::data),
		const uint8_t*>,
		"ProtocolFrame::data is a BORROWED const buffer. A non-const pointer "
		"would invite the hub to write through it.");
	static_assert(std::is_same_v<decltype(g2::ProtocolFrame::size), size_t>,
		"ProtocolFrame::size is a size_t.");
	static_assert(std::is_same_v<decltype(g2::StampedFrame::frameIndex),
		uint64_t>,
		"StampedFrame::frameIndex is the quantum at which the frame entered.");
	static_assert(std::is_same_v<decltype(g2::StampedFrame::frame),
		g2::ProtocolFrame>,
		"StampedFrame carries a whole ProtocolFrame.");

	/* THE ENDPOINT COUNT IS READ FROM THE PRODUCTION DECLARATION, and the
	 * whole allocation assertion below is built on it. Section 15.1 fixes the
	 * three attachments; this test names none of them and copies the count
	 * from nowhere. */
	static_assert(g2::TransportHub::kMaxEndpoints >= 2,
		"The fixed endpoint order is only observable with at least two "
		"endpoints, and this test drives kMaxEndpoints of them.");

	/* ---------------------------------------------------------------- the
	 * two sizes this test constructs with.
	 *
	 * THE CEILING IS DERIVED, NOT COPIED. Section 15.3 frames a message as
	 * [1-byte type][2-byte length][payload], so the largest frame the wire can
	 * carry is one type byte, two length bytes, and the largest payload a
	 * two-byte length can name. Writing 65538 here would be the same defect as
	 * writing 3 for the endpoint count. */
	constexpr size_t kDerivedCeiling = 1u + 2u
		+ static_cast<size_t>(std::numeric_limits<uint16_t>::max());

	/* queueDepth is an ordinary tunable and is this test's own INPUT, not a
	 * copy of production state: it is passed to the constructor on the line
	 * below and read back nowhere. */
	constexpr size_t kQueueDepth = 16;

	constexpr size_t allocationFor(const size_t maxFrameBytes,
		const size_t queueDepth)
	{
		return g2::TransportHub::kMaxEndpoints * queueDepth
			* (sizeof(g2::StampedFrame) + maxFrameBytes);
	}

	/* ---------------------------------------------------------------- an
	 * endpoint that records what the hub hands it, WITHOUT ALLOCATING, so that
	 * it can live inside an armed window. */
	constexpr size_t kRecordCapacity = 256;
	constexpr size_t kRecordBytes    = 64;

	class RecordingEndpoint final : public g2::TransportEndpoint
	{
	public:
		void onFrameFromDevice(const g2::ProtocolFrame f) noexcept override
		{
			if(m_count >= kRecordCapacity || f.size > kRecordBytes)
			{
				++m_refusedByFixture;
				return;
			}
			m_size[m_count] = f.size;
			for(size_t i = 0; i < f.size; ++i)
				m_data[m_count][i] = f.data[i];
			++m_count;
		}

		size_t         count() const           { return m_count; }
		size_t         sizeAt(const size_t i) const { return m_size[i]; }
		const uint8_t* dataAt(const size_t i) const { return m_data[i]; }
		size_t         refusedByFixture() const { return m_refusedByFixture; }
		void           clear()                 { m_count = 0; }

	private:
		size_t  m_count = 0;
		size_t  m_refusedByFixture = 0;
		size_t  m_size[kRecordCapacity] = {};
		uint8_t m_data[kRecordCapacity][kRecordBytes] = {};
	};

	/* A payload whose every byte is derived from one seed, so that "the frame
	 * that came out is the frame that went in" is a statement about every byte
	 * and not about the first one. */
	void fillPattern(uint8_t* const dst, const size_t size, const uint32_t seed)
	{
		for(size_t i = 0; i < size; ++i)
			dst[i] = static_cast<uint8_t>((seed * 131u + i * 17u + 7u) & 0xffu);
	}

	bool matchesPattern(const uint8_t* const src, const size_t size,
		const uint32_t seed)
	{
		for(size_t i = 0; i < size; ++i)
		{
			if(src[i] != static_cast<uint8_t>((seed * 131u + i * 17u + 7u) & 0xffu))
				return false;
		}
		return true;
	}

	/* ---------------------------------------------------------------- case 3:
	 * the declaration order, read from the header's own source text. */
	void checkDeclarationOrder(const char* const headerPath)
	{
		std::FILE* const f = std::fopen(headerPath, "rb");
		if(f == nullptr)
		{
			printf("FAIL the header is readable at the path argv[1] names: %s\n",
				headerPath);
			++failures;
			return;
		}

		std::string text;
		char buffer[4096];
		size_t got = 0;
		while((got = std::fread(buffer, 1, sizeof(buffer), f)) != 0)
			text.append(buffer, got);
		std::fclose(f);

		check(!text.empty(), "the header the order case reads is not empty");

		/* THE TOKENS ARE THE DEFINITIONS AND NOT THE NAMES, and that is the
		 * load-bearing part of this case. A forward declaration --
		 * `struct StampedFrame;` before the class, with the body after it --
		 * compiles, links and satisfies every other assertion in this file,
		 * and it is EXACTLY the earlier draft's shape that design section
		 * 13.10.6 records. Comparing the position of the DEFINITION is what
		 * tells the two apart; comparing the position of the name does not. */
		const std::string protocolFrame = "struct ProtocolFrame\n\t{";
		const std::string stampedFrame  = "struct StampedFrame\n\t{";
		const std::string transportHub  = "class TransportHub\n\t{";

		const size_t atProtocol  = text.find(protocolFrame);
		const size_t atStamped   = text.find(stampedFrame);
		const size_t atHub       = text.find(transportHub);

		/* THE KNOWN-NEGATIVE CONTROL. Without it, "not found" and "the file
		 * was never really searched" produce the same three npos values and
		 * every assertion below would be an absence proof resting on nothing. */
		const size_t atAbsent = text.find("struct ThisDeclarationDoesNotExist");
		check(atAbsent == std::string::npos,
			"the scanner reports a token that is not in the header as absent "
			"(the known-negative control for the three searches below)");

		check(atProtocol != std::string::npos,
			"the header DEFINES `struct ProtocolFrame`");
		check(atStamped != std::string::npos,
			"the header DEFINES `struct StampedFrame`");
		check(atHub != std::string::npos,
			"the header DEFINES `class TransportHub`");

		if(atProtocol == std::string::npos || atStamped == std::string::npos
			|| atHub == std::string::npos)
			return;

		check(atProtocol < atHub,
			"ProtocolFrame is declared BEFORE TransportHub");
		check(atStamped < atHub,
			"StampedFrame is declared BEFORE TransportHub -- drainToDevice "
			"fills an out-array of it and an earlier draft placed it after");

		check(text.find(protocolFrame, atProtocol + 1) == std::string::npos,
			"`struct ProtocolFrame` is defined exactly once");
		check(text.find(stampedFrame, atStamped + 1) == std::string::npos,
			"`struct StampedFrame` is defined exactly once");
		check(text.find(transportHub, atHub + 1) == std::string::npos,
			"`class TransportHub` is defined exactly once");

		/* AND NO FORWARD DECLARATION STANDS IN FOR EITHER DEFINITION. Without
		 * this pair, a header that forward-declares the struct above the class
		 * and defines it below satisfies the two order assertions above while
		 * being the very draft they exist to refuse. */
		check(text.find("struct ProtocolFrame;") == std::string::npos,
			"no forward declaration of ProtocolFrame stands in for its "
			"definition above TransportHub");
		check(text.find("struct StampedFrame;") == std::string::npos,
			"no forward declaration of StampedFrame stands in for its "
			"definition above TransportHub");
	}

	/* ---------------------------------------------------------------- case 1:
	 * the fixed allocation total, measured at two different pairs of sizes so
	 * that the assertion is against the FORMULA and not against one product. */
	void checkFixedAllocation(const size_t maxFrameBytes, const size_t queueDepth,
		const char* const what)
	{
		armAlloc();
		g2::TransportHub hub(maxFrameBytes, queueDepth);
		disarmAlloc();

		const uint64_t bytes = g_alloc.bytes;
		const uint64_t calls = g_alloc.calls;

		checkEqual(bytes, allocationFor(maxFrameBytes, queueDepth), what);
		check(calls != 0,
			"the constructor allocates at all -- a hub that allocated nothing "
			"at construction would satisfy the byte total only by matching "
			"zero, which no non-degenerate size pair does");

		/* The hub must be USED after the measurement, or a constructor that
		 * allocated the right number of bytes and threw them away would pass. */
		RecordingEndpoint endpoint;
		hub.attach(endpoint);
		uint8_t payload[8];
		fillPattern(payload, sizeof(payload), 5u);
		check(hub.toDevice(endpoint, g2::ProtocolFrame{ payload, sizeof(payload) }),
			"the measured hub accepts a frame after the allocation window "
			"closes");
		hub.detach(endpoint);
	}
}

int main(const int argc, const char* const* const argv)
{
	/* UNBUFFERED, AND THE REASON IS A MEASUREMENT. A mutation that made the
	 * constructor under-allocate crashed this program later on, and every FAIL
	 * line the earlier cases had already printed was lost with the buffer -- so
	 * the run reported a bare SIGTRAP and said nothing about which assertion
	 * had already caught the defect. A verdict that is only readable if the
	 * program survives to exit is a verdict the worst failures delete. */
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	printf("t0_transport_hub: kMaxEndpoints=%zu sizeof(StampedFrame)=%zu "
		"derivedCeiling=%zu queueDepth=%zu expectedBytes=%zu\n",
		g2::TransportHub::kMaxEndpoints, sizeof(g2::StampedFrame),
		kDerivedCeiling, kQueueDepth,
		allocationFor(kDerivedCeiling, kQueueDepth));

	/* ---------------- case 3: the declaration order. */
	if(argc < 2)
	{
		printf("FAIL the header path arrives on argv[1]; the registration "
			"passes it and a guessed path is a path this check could get "
			"wrong in silence\n");
		++failures;
	}
	else
	{
		checkDeclarationOrder(argv[1]);
	}

	/* ---------------- case 1: the fixed allocation, at the derived ceiling
	 * and at a second, unrelated pair of sizes. */
	checkFixedAllocation(kDerivedCeiling, kQueueDepth,
		"the hub's construction allocates exactly "
		"kMaxEndpoints x queueDepth x (sizeof(StampedFrame) + maxFrameBytes) "
		"at the derived ceiling");
	checkFixedAllocation(128u, 4u,
		"the hub's construction allocates exactly "
		"kMaxEndpoints x queueDepth x (sizeof(StampedFrame) + maxFrameBytes) "
		"at a second, unrelated pair of sizes");

	/* ---------------- case 2: nothing allocates after construction.
	 *
	 * Every object the window touches is constructed BEFORE the window opens,
	 * and every endpoint in it carries fixed arrays, so a non-zero count
	 * inside the window belongs to the hub.
	 *
	 * THE STEADY RATE IS kPerQuantum AND NOT queueDepth, AND THE REASON IS A
	 * REAL PROPERTY OF THE BORROW CONTRACT RATHER THAN A CONVENIENCE. The
	 * frames one drain hands out stay borrowed until the NEXT drain, and they
	 * sit in the very slots a producer would otherwise refill, so the depth a
	 * producer can use inside one quantum is queueDepth MINUS whatever the
	 * last drain lent out. Driving queueDepth frames on every quantum would
	 * therefore alternate between full and fully blocked, and an expectation
	 * written as `200 x endpoints x queueDepth` would be a claim the contract
	 * contradicts. The burst phase below asserts that borrow cost EXACTLY. */
	{
		constexpr size_t kFrameBytes = 32;
		constexpr size_t kQuanta     = 200;
		constexpr size_t kPerQuantum = 2;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint endpoints[g2::TransportHub::kMaxEndpoints];
		uint8_t           payload[kFrameBytes];
		uint8_t           oversize[kFrameBytes + 1];
		std::vector<g2::StampedFrame> out(
			g2::TransportHub::kMaxEndpoints * kQueueDepth);
		fillPattern(payload, sizeof(payload), 11u);
		fillPattern(oversize, sizeof(oversize), 12u);

		size_t accepted      = 0;
		size_t oversized     = 0;
		size_t drained       = 0;
		size_t burstAccepted = 0;
		size_t burstRefused  = 0;

		armAlloc();
		for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
			hub.attach(endpoints[e]);

		for(size_t quantum = 0; quantum < kQuanta; ++quantum)
		{
			for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
			{
				for(size_t k = 0; k < kPerQuantum; ++k)
				{
					if(hub.toDevice(endpoints[e],
						g2::ProtocolFrame{ payload, sizeof(payload) }))
						++accepted;
				}
				/* Past the size boundary on purpose: the refusal path must not
				 * allocate either. */
				if(!hub.toDevice(endpoints[e],
					g2::ProtocolFrame{ oversize, sizeof(oversize) }))
					++oversized;
			}

			hub.fromDevice(g2::ProtocolFrame{ payload, sizeof(payload) });
			drained += hub.drainToDevice(out.data(), out.size());
		}

		/* THE BURST, still inside the window: the depth-refusal path must not
		 * allocate either, and its exact accept count is the borrow cost. */
		for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
		{
			for(size_t k = 0; k < kQueueDepth + 2; ++k)
			{
				if(hub.toDevice(endpoints[e],
					g2::ProtocolFrame{ payload, sizeof(payload) }))
					++burstAccepted;
				else
					++burstRefused;
			}
		}

		for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
			hub.detach(endpoints[e]);
		disarmAlloc();

		checkEqual(g_alloc.calls, 0u,
			"nothing allocates after construction: attach, toDevice at and "
			"past both refusal boundaries, fromDevice, drainToDevice and "
			"detach make ZERO allocation calls over 200 quanta");
		checkEqual(g_alloc.bytes, 0u,
			"nothing allocates after construction: the same window allocates "
			"ZERO bytes");

		/* The window must have DONE something, or a hub whose every method
		 * returned immediately would pass the two assertions above. */
		checkEqual(accepted,
			kQuanta * g2::TransportHub::kMaxEndpoints * kPerQuantum,
			"the no-allocation window accepted every frame it offered inside "
			"the steady rate, for each endpoint on each of its 200 quanta");
		checkEqual(oversized, kQuanta * g2::TransportHub::kMaxEndpoints,
			"the no-allocation window refused the one oversize frame for each "
			"endpoint on each quantum");
		checkEqual(drained, accepted,
			"the no-allocation window drained every frame it accepted");
		checkEqual(burstAccepted,
			g2::TransportHub::kMaxEndpoints * (kQueueDepth - kPerQuantum),
			"a burst offered right after a drain is accepted up to queueDepth "
			"MINUS the frames that drain lent out, because a borrowed slot "
			"cannot be refilled before the borrow expires");
		checkEqual(burstRefused,
			g2::TransportHub::kMaxEndpoints * (kQueueDepth + 2u
				- (kQueueDepth - kPerQuantum)),
			"the burst frames past that point are refused, and the refusal "
			"path allocates nothing either");
		for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
		{
			checkEqual(endpoints[e].count(), kQuanta,
				"each attached endpoint received one fromDevice frame on each "
				"of the 200 quanta");
			checkEqual(endpoints[e].refusedByFixture(), 0u,
				"the recording fixture refused nothing, so its own capacity "
				"never stood in for the hub's behaviour");
		}
	}

	/* ---------------- case 4a: a frame larger than maxFrameBytes is refused,
	 * and the largest permitted frame is NOT. */
	{
		constexpr size_t kFrameBytes = 40;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint endpoint;
		hub.attach(endpoint);

		uint8_t exact[kFrameBytes];
		uint8_t oversize[kFrameBytes + 1];
		fillPattern(exact, sizeof(exact), 21u);
		fillPattern(oversize, sizeof(oversize), 22u);

		check(hub.toDevice(endpoint,
			g2::ProtocolFrame{ exact, sizeof(exact) }),
			"a frame of exactly maxFrameBytes is ACCEPTED -- the refusal is "
			"strictly greater-than and not greater-or-equal");
		check(!hub.toDevice(endpoint,
			g2::ProtocolFrame{ oversize, sizeof(oversize) }),
			"a frame of maxFrameBytes + 1 is refused by toDevice");

		std::vector<g2::StampedFrame> out(kQueueDepth);
		checkEqual(hub.drainToDevice(out.data(), out.size()), 1u,
			"only the accepted frame reached the device; the refused one was "
			"not queued");
		checkEqual(out[0].frame.size, kFrameBytes,
			"the accepted frame kept its size");
		check(matchesPattern(out[0].frame.data, out[0].frame.size, 21u),
			"the accepted frame kept every one of its bytes");
	}

	/* ---------------- case 4b: an endpoint queue filled to queueDepth makes
	 * toDevice return false, and the queued frames survive. */
	{
		constexpr size_t kFrameBytes = 24;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint a;
		RecordingEndpoint b;
		hub.attach(a);
		hub.attach(b);

		uint8_t payload[kQueueDepth + 1][kFrameBytes];
		for(size_t k = 0; k < kQueueDepth + 1; ++k)
			fillPattern(payload[k], kFrameBytes, static_cast<uint32_t>(100 + k));

		for(size_t k = 0; k < kQueueDepth; ++k)
		{
			check(hub.toDevice(a, g2::ProtocolFrame{ payload[k], kFrameBytes }),
				"the first queueDepth frames are accepted");
		}
		check(!hub.toDevice(a,
			g2::ProtocolFrame{ payload[kQueueDepth], kFrameBytes }),
			"an endpoint queue filled to queueDepth makes toDevice return "
			"false");

		/* THE REFUSAL IS PER ENDPOINT. A hub with one shared queue would
		 * refuse here too, and this case is what tells the two apart. */
		check(hub.toDevice(b, g2::ProtocolFrame{ payload[0], kFrameBytes }),
			"a full queue on one endpoint does not refuse another endpoint: "
			"the depth is per endpoint");

		std::vector<g2::StampedFrame> out(2u * kQueueDepth);
		const size_t got = hub.drainToDevice(out.data(), out.size());
		checkEqual(got, kQueueDepth + 1u,
			"the drain returned the queueDepth frames of the full endpoint "
			"plus the one frame of the other, and no copy of the refused one");
		for(size_t k = 0; k < kQueueDepth; ++k)
		{
			check(matchesPattern(out[k].frame.data, out[k].frame.size,
				static_cast<uint32_t>(100 + k)),
				"a refused frame overwrote no frame already queued");
		}
	}

	/* ---------------- case 5: the fixed attachment order and the stamp. */
	{
		constexpr size_t kFrameBytes = 16;
		constexpr size_t kQuanta     = 100;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint endpoints[g2::TransportHub::kMaxEndpoints];
		for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
			hub.attach(endpoints[e]);

		std::vector<g2::StampedFrame> out(
			g2::TransportHub::kMaxEndpoints * kQueueDepth);

		for(size_t quantum = 0; quantum < kQuanta; ++quantum)
		{
			/* Every endpoint delivers in the SAME quantum, which is the case
			 * the fixed order exists for. The seed encodes the endpoint, so
			 * "in attachment order" is a statement about which endpoint's
			 * bytes landed at which out-index. */
			uint8_t payload[g2::TransportHub::kMaxEndpoints][kFrameBytes];
			for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
			{
				fillPattern(payload[e], kFrameBytes,
					static_cast<uint32_t>(1000 + e));
				check(hub.toDevice(endpoints[e],
					g2::ProtocolFrame{ payload[e], kFrameBytes }),
					"each endpoint delivered one frame into the quantum");
			}

			const size_t got = hub.drainToDevice(out.data(), out.size());
			checkEqual(got, g2::TransportHub::kMaxEndpoints,
				"the drain returned one frame for each attached endpoint");

			for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
			{
				check(matchesPattern(out[e].frame.data, out[e].frame.size,
					static_cast<uint32_t>(1000 + e)),
					"drainToDevice visits endpoints in ATTACHMENT ORDER: the "
					"frame at out-index i carries the bytes the i-th attached "
					"endpoint delivered");
				checkEqual(out[e].frameIndex, quantum,
					"every frame drained in a quantum is stamped with that "
					"quantum's frame index, and the index advances by exactly "
					"one for each drainToDevice");
			}

			if(failures != 0)
				break;
		}
	}

	/* ---------------- case 5b: detaching the FIRST endpoint leaves the order
	 * of the rest, so the order is attachment order and not call order. */
	{
		constexpr size_t kFrameBytes = 16;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint a;
		RecordingEndpoint b;
		RecordingEndpoint c;
		hub.attach(a);
		hub.attach(b);
		hub.attach(c);
		hub.detach(a);

		uint8_t payload[2][kFrameBytes];
		fillPattern(payload[0], kFrameBytes, 31u);
		fillPattern(payload[1], kFrameBytes, 32u);

		/* Delivered in REVERSE attachment order on purpose. */
		check(hub.toDevice(c, g2::ProtocolFrame{ payload[1], kFrameBytes }),
			"the last-attached endpoint delivered first");
		check(hub.toDevice(b, g2::ProtocolFrame{ payload[0], kFrameBytes }),
			"the earlier-attached endpoint delivered second");

		std::vector<g2::StampedFrame> out(4);
		checkEqual(hub.drainToDevice(out.data(), out.size()), 2u,
			"the detached endpoint contributed nothing and the two attached "
			"ones contributed one frame each");
		check(matchesPattern(out[0].frame.data, out[0].frame.size, 31u),
			"the drain order is ATTACHMENT order and not the order the frames "
			"were delivered in");
		check(matchesPattern(out[1].frame.data, out[1].frame.size, 32u),
			"the second drained frame is the later-attached endpoint's");

		check(!hub.toDevice(a, g2::ProtocolFrame{ payload[0], kFrameBytes }),
			"a detached endpoint is refused by toDevice");
	}

	/* ---------------- case 6: the borrow lifetime.
	 *
	 * The pointers one drainToDevice returns are read IMMEDIATELY BEFORE the
	 * next drainToDevice, AFTER further traffic has run against the same
	 * endpoint. A hub that released its slots at the end of the drain would
	 * have overwritten them by then, and this case is what sees that. */
	{
		constexpr size_t kFrameBytes = 16;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint endpoint;
		hub.attach(endpoint);

		std::vector<g2::StampedFrame> out(kQueueDepth);
		std::vector<g2::StampedFrame> later(kQueueDepth);
		uint8_t payload[kFrameBytes];

		fillPattern(payload, kFrameBytes, 77u);
		check(hub.toDevice(endpoint, g2::ProtocolFrame{ payload, kFrameBytes }),
			"the borrowed frame was accepted");
		checkEqual(hub.drainToDevice(out.data(), out.size()), 1u,
			"the borrowed frame was drained");

		const uint8_t* const borrowed = out[0].frame.data;
		const size_t         borrowedSize = out[0].frame.size;

		/* Traffic against the same endpoint, inside the same quantum, filling
		 * the whole ring behind the borrowed slot. */
		for(size_t k = 0; k < kQueueDepth; ++k)
		{
			uint8_t other[kFrameBytes];
			fillPattern(other, kFrameBytes, static_cast<uint32_t>(200 + k));
			hub.toDevice(endpoint, g2::ProtocolFrame{ other, kFrameBytes });
		}

		checkEqual(borrowedSize, kFrameBytes,
			"the borrowed frame kept its size");
		check(matchesPattern(borrowed, borrowedSize, 77u),
			"the pointers one drainToDevice returns are still readable "
			"immediately before the NEXT drainToDevice, after further "
			"toDevice traffic against the same endpoint");

		hub.drainToDevice(later.data(), later.size());
	}

	/* ---------------- case 7: fromDevice reaches every attached endpoint, in
	 * attachment order, and no detached one. */
	{
		constexpr size_t kFrameBytes = 12;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint a;
		RecordingEndpoint b;
		RecordingEndpoint gone;
		hub.attach(a);
		hub.attach(b);
		hub.attach(gone);
		hub.detach(gone);

		uint8_t payload[kFrameBytes];
		fillPattern(payload, kFrameBytes, 55u);
		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });

		checkEqual(a.count(), 1u, "fromDevice reached the first attachment");
		checkEqual(b.count(), 1u, "fromDevice reached the second attachment");
		checkEqual(gone.count(), 0u,
			"fromDevice reached no detached attachment");
		check(matchesPattern(a.dataAt(0), a.sizeAt(0), 55u),
			"the first attachment received every byte of the frame");
		check(matchesPattern(b.dataAt(0), b.sizeAt(0), 55u),
			"the second attachment received every byte of the frame");
	}

	/* ---------------- case 8: `max` truncates the drain and leaves the rest
	 * queued. The parameter is in the declared signature, so its behaviour is
	 * part of the contract. */
	{
		constexpr size_t kFrameBytes = 8;
		g2::TransportHub  hub(kFrameBytes, kQueueDepth);
		RecordingEndpoint endpoint;
		hub.attach(endpoint);

		uint8_t payload[4][kFrameBytes];
		for(size_t k = 0; k < 4; ++k)
			fillPattern(payload[k], kFrameBytes, static_cast<uint32_t>(300 + k));
		for(size_t k = 0; k < 4; ++k)
			hub.toDevice(endpoint, g2::ProtocolFrame{ payload[k], kFrameBytes });

		std::vector<g2::StampedFrame> out(4);
		checkEqual(hub.drainToDevice(out.data(), 2u), 2u,
			"drainToDevice returns no more than `max` frames");
		check(matchesPattern(out[0].frame.data, out[0].frame.size, 300u),
			"the truncated drain returned the oldest frame first");
		check(matchesPattern(out[1].frame.data, out[1].frame.size, 301u),
			"the truncated drain returned the second-oldest frame second");

		checkEqual(hub.drainToDevice(out.data(), out.size()), 2u,
			"the frames `max` left behind are still queued for the next "
			"drain and are not dropped");
		check(matchesPattern(out[0].frame.data, out[0].frame.size, 302u),
			"the next drain resumed where the truncated one stopped");
		checkEqual(out[0].frameIndex, 1u,
			"the frames the second drain returned carry the SECOND quantum's "
			"index, because the stamp is the quantum a frame was drained in");
	}

	if(failures != 0)
	{
		printf("t0_transport_hub: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_transport_hub: all cases passed\n");
	return 0;
}
