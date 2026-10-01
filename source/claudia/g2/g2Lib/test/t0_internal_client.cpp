/* t0_internal_client.cpp -- the internal protocol client.
 * Tier T0: no artifact, no firmware, no file outside this repository.
 *
 * The property this file exists to hold: the internal client is a peer of the
 * usbip endpoint, not a path through it. The hub in case 1 carries one
 * attachment -- this client -- and nothing else exists to route through, so a
 * client that reached its device through another endpoint could not deliver a
 * single byte there. The compile-time half is case 2: InternalClient derives
 * from g2::TransportEndpoint directly, and is constructible from a hub
 * reference alone, so no other attachment can be a constructor argument.
 *
 * Nothing in this file is an assert() and nothing catches an exception. The
 * default build type of this repository is Release and Release defines NDEBUG,
 * which deletes every assert(); every run-time verdict here reports through
 * this file's own failure counter, and every compile-time verdict is a
 * static_assert, which fires in every build type.
 */
#include "../internalClient.h"

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

	/* A byte pattern that depends on the seed AND on the position, so that a
	 * frame delivered out of order, a frame truncated, and a frame whose
	 * pointer went stale are three different failures rather than one. */
	void fillPattern(uint8_t* const dst, const size_t size, const uint32_t seed)
	{
		for(size_t i = 0; i < size; ++i)
			dst[i] = static_cast<uint8_t>(seed * 31u + i * 7u + 1u);
	}

	bool matchesPattern(const uint8_t* const data, const size_t size,
		const size_t expectedSize, const uint32_t seed)
	{
		if(data == nullptr || size != expectedSize)
			return false;
		for(size_t i = 0; i < size; ++i)
		{
			if(data[i] != static_cast<uint8_t>(seed * 31u + i * 7u + 1u))
				return false;
		}
		return true;
	}

	void checkFrame(const g2::ProtocolFrame& frame, const size_t expectedSize,
		const uint32_t seed, const char* const what)
	{
		check(matchesPattern(frame.data, frame.size, expectedSize, seed), what);
	}

	/* An endpoint with no container of its own, for case 9. */
	class RecordingEndpoint final : public g2::TransportEndpoint
	{
	public:
		void onFrameFromDevice(const g2::ProtocolFrame) noexcept override
		{
			++m_count;
		}

		uint64_t count() const noexcept { return m_count; }

	private:
		uint64_t m_count = 0;
	};

	/* The allocation counter. `armed` is true only across a measured window. */
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

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

/* ---------------- case 2: the compile-time half of "peer, not a path".
 *
 * The base-class assertion is the load-bearing one. A client implemented as a
 * layer over another attachment would hold or wrap that attachment; a client
 * that IS an attachment derives from the interface the hub knows, exactly as
 * the socket and usbip attachments will. is_constructible pins the second
 * half: a hub reference and two sizes are the whole of what it needs, so no
 * other endpoint can be threaded through the constructor. */
static_assert(std::is_base_of<g2::TransportEndpoint, g2::InternalClient>::value,
	"InternalClient must BE a transport endpoint, not own one");
static_assert(std::is_constructible<g2::InternalClient, g2::TransportHub&, size_t, size_t>::value,
	"InternalClient is constructed from a hub and its own two sizes, and from nothing else");
static_assert(!std::is_copy_constructible<g2::InternalClient>::value,
	"an attached endpoint has a hub position and cannot be copied");
static_assert(noexcept(std::declval<g2::InternalClient&>().onFrameFromDevice(g2::ProtocolFrame{ nullptr, 0 })),
	"the scheduler-thread callback is noexcept");

int main()
{
	constexpr size_t kFrameBytes = 8;
	constexpr size_t kQueueDepth = 2;

	/* ---------------- case 1: a peer, not a path.
	 *
	 * One hub, one attachment, no editor, no USB stack, nothing to route
	 * through. The client originates a frame and it reaches the device. */
	{
		g2::TransportHub   hub(kFrameBytes, kQueueDepth);
		g2::InternalClient client(hub, kFrameBytes, kQueueDepth);

		uint8_t payload[kFrameBytes];
		fillPattern(payload, kFrameBytes, 11u);

		check(client.send(g2::ProtocolFrame{ payload, kFrameBytes }),
			"the client originates a frame with no other attachment present");

		g2::StampedFrame out[2];
		checkEqual(hub.drainToDevice(out, 2u), 1u,
			"the originated frame reached the device on the next quantum");
		checkFrame(out[0].frame, kFrameBytes, 11u,
			"the device received every byte the client originated");
		checkEqual(out[0].frameIndex, 0u,
			"the originated frame carries the quantum it was drained in");
	}

	/* ---------------- case 3: the inbox COPIES what the device delivers.
	 *
	 * The delivered buffer is overwritten after the callback has returned and
	 * before the frame is consumed. */
	{
		g2::TransportHub   hub(kFrameBytes, kQueueDepth);
		g2::InternalClient client(hub, kFrameBytes, kQueueDepth);

		uint8_t payload[kFrameBytes];
		fillPattern(payload, kFrameBytes, 21u);
		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });

		fillPattern(payload, kFrameBytes, 99u);

		g2::ProtocolFrame got{ nullptr, 0 };
		check(client.receive(got), "the delivered frame is available to the plugin");
		checkFrame(got, kFrameBytes, 21u,
			"the inbox holds the bytes as delivered, not the overwritten buffer");
		checkEqual(client.droppedFrames(), 0u, "nothing was dropped");
	}

	/* ---------------- case 4: FIFO. */
	{
		g2::TransportHub   hub(kFrameBytes, 4u);
		g2::InternalClient client(hub, kFrameBytes, 4u);

		for(uint32_t k = 0; k < 3u; ++k)
		{
			uint8_t payload[kFrameBytes];
			fillPattern(payload, kFrameBytes, 40u + k);
			hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });
		}

		g2::ProtocolFrame got{ nullptr, 0 };
		check(client.receive(got), "the first of three frames is available");
		checkFrame(got, kFrameBytes, 40u, "the oldest frame came out first");
		check(client.receive(got), "the second of three frames is available");
		checkFrame(got, kFrameBytes, 41u, "the second frame came out second");
		check(client.receive(got), "the third of three frames is available");
		checkFrame(got, kFrameBytes, 42u, "the newest frame came out last");
		check(!client.receive(got), "an emptied inbox reports empty");
	}

	/* ---------------- case 5: a full inbox drops the NEW frame and counts it. */
	{
		g2::TransportHub   hub(kFrameBytes, kQueueDepth);
		g2::InternalClient client(hub, kFrameBytes, 2u);

		for(uint32_t k = 0; k < 3u; ++k)
		{
			uint8_t payload[kFrameBytes];
			fillPattern(payload, kFrameBytes, 50u + k);
			hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });
		}

		checkEqual(client.droppedFrames(), 1u,
			"the frame that did not fit was counted");

		g2::ProtocolFrame got{ nullptr, 0 };
		check(client.receive(got), "the first queued frame survived the overflow");
		checkFrame(got, kFrameBytes, 50u,
			"the overflow dropped the NEW frame and not the oldest queued one");
		check(client.receive(got), "the second queued frame survived the overflow");
		checkFrame(got, kFrameBytes, 51u, "the second queued frame is intact");
		check(!client.receive(got), "the dropped frame was not queued after all");

		/* The depth returns once the frames are consumed and released. */
		uint8_t payload[kFrameBytes];
		fillPattern(payload, kFrameBytes, 59u);
		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });
		checkEqual(client.droppedFrames(), 1u,
			"a consumed inbox takes a new frame and drops nothing further");
		check(client.receive(got), "the frame delivered after the drain is available");
		checkFrame(got, kFrameBytes, 59u, "and it is the frame that was delivered");
	}

	/* ---------------- case 6: an oversized delivery is dropped and counted. */
	{
		g2::TransportHub   hub(kFrameBytes * 2u, kQueueDepth);
		g2::InternalClient client(hub, kFrameBytes, kQueueDepth);

		uint8_t payload[kFrameBytes + 1u];
		fillPattern(payload, kFrameBytes + 1u, 61u);
		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes + 1u });

		checkEqual(client.droppedFrames(), 1u,
			"a frame larger than the client's maxFrameBytes was counted");

		g2::ProtocolFrame got{ nullptr, 0 };
		check(!client.receive(got),
			"the oversized frame was dropped whole and not truncated into the inbox");

		/* The boundary itself fits. */
		uint8_t exact[kFrameBytes];
		fillPattern(exact, kFrameBytes, 62u);
		hub.fromDevice(g2::ProtocolFrame{ exact, kFrameBytes });
		check(client.receive(got), "a frame of exactly maxFrameBytes is accepted");
		checkFrame(got, kFrameBytes, 62u, "and it arrives whole");
		checkEqual(client.droppedFrames(), 1u, "and it is not counted as a drop");
	}

	/* ---------------- case 7: the hub's two refusals reach the caller. */
	{
		g2::TransportHub   hub(kFrameBytes, kQueueDepth);
		g2::InternalClient client(hub, kFrameBytes, kQueueDepth);

		uint8_t tooBig[kFrameBytes + 1u];
		fillPattern(tooBig, kFrameBytes + 1u, 71u);
		check(!client.send(g2::ProtocolFrame{ tooBig, kFrameBytes + 1u }),
			"send reports the hub's refusal of an oversized frame");

		uint8_t payload[kFrameBytes];
		fillPattern(payload, kFrameBytes, 72u);
		check(client.send(g2::ProtocolFrame{ payload, kFrameBytes }),
			"the first frame fits the hub's queue");
		check(client.send(g2::ProtocolFrame{ payload, kFrameBytes }),
			"the second frame fits the hub's queue");
		check(!client.send(g2::ProtocolFrame{ payload, kFrameBytes }),
			"send reports the hub's refusal when the queue is full");

		g2::StampedFrame out[4];
		checkEqual(hub.drainToDevice(out, 4u), 2u,
			"exactly the two accepted frames reached the device");
	}

	/* ---------------- case 8: the receive borrow lifetime.
	 *
	 * What receive() handed out stays readable until the NEXT receive, across
	 * further deliveries -- and the slot it occupies is unavailable to the
	 * producer until then, which is why the third delivery is dropped rather
	 * than overwriting a frame the plugin is still reading. */
	{
		g2::TransportHub   hub(kFrameBytes, kQueueDepth);
		g2::InternalClient client(hub, kFrameBytes, 2u);

		uint8_t first[kFrameBytes];
		fillPattern(first, kFrameBytes, 81u);
		hub.fromDevice(g2::ProtocolFrame{ first, kFrameBytes });

		g2::ProtocolFrame borrowed{ nullptr, 0 };
		check(client.receive(borrowed), "the first frame is available");
		checkFrame(borrowed, kFrameBytes, 81u, "and it is the frame delivered");

		for(uint32_t k = 0; k < 2u; ++k)
		{
			uint8_t payload[kFrameBytes];
			fillPattern(payload, kFrameBytes, 82u + k);
			hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });
		}

		checkFrame(borrowed, kFrameBytes, 81u,
			"the borrowed frame is intact after further deliveries, because it "
			"is released at the NEXT receive and not at the end of the last one");
		checkEqual(client.droppedFrames(), 1u,
			"the delivery that needed the borrowed slot was dropped, not written over it");

		g2::ProtocolFrame got{ nullptr, 0 };
		check(client.receive(got), "the next frame is available");
		checkFrame(got, kFrameBytes, 82u, "and it is the one that fitted");
	}

	/* ---------------- case 9: the destructor detaches.
	 *
	 * kMaxEndpoints is read from the hub, not written here. */
	{
		g2::TransportHub hub(kFrameBytes, kQueueDepth);

		{
			g2::InternalClient client(hub, kFrameBytes, kQueueDepth);
			uint8_t payload[kFrameBytes];
			fillPattern(payload, kFrameBytes, 91u);
			check(client.send(g2::ProtocolFrame{ payload, kFrameBytes }),
				"the client held a hub position while it was alive");
		}

		RecordingEndpoint later[g2::TransportHub::kMaxEndpoints];
		for(auto& e : later)
			hub.attach(e);

		uint8_t payload[kFrameBytes];
		fillPattern(payload, kFrameBytes, 92u);
		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });

		for(size_t e = 0; e < g2::TransportHub::kMaxEndpoints; ++e)
			checkEqual(later[e].count(), 1u,
				"every hub position took an attachment after the client died");
	}

	/* ---------------- case 10: nothing allocates after construction. */
	{
		g2::TransportHub   hub(kFrameBytes, kQueueDepth);
		g2::InternalClient client(hub, kFrameBytes, kQueueDepth);

		uint8_t payload[kFrameBytes];
		fillPattern(payload, kFrameBytes, 101u);
		uint8_t tooBig[kFrameBytes + 1u];
		fillPattern(tooBig, kFrameBytes + 1u, 102u);
		g2::StampedFrame  out[4];
		g2::ProtocolFrame got{ nullptr, 0 };

		armAlloc();

		client.send(g2::ProtocolFrame{ payload, kFrameBytes });
		client.send(g2::ProtocolFrame{ tooBig, kFrameBytes + 1u });
		hub.drainToDevice(out, 4u);

		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });
		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });
		hub.fromDevice(g2::ProtocolFrame{ payload, kFrameBytes });
		hub.fromDevice(g2::ProtocolFrame{ tooBig, kFrameBytes + 1u });

		client.receive(got);
		client.receive(got);
		client.receive(got);
		client.droppedFrames();

		const AllocStats window = g_alloc;
		disarmAlloc();

		checkEqual(window.calls, 0u,
			"no allocation call was made after the client was constructed");
		checkEqual(window.bytes, 0u,
			"no byte was allocated after the client was constructed");
	}

	if(failures != 0)
	{
		printf("t0_internal_client: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_internal_client: all cases passed\n");
	return 0;
}
