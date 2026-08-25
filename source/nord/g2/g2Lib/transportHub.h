/* transportHub.h -- g2::TransportHub and the two frame types it moves.
 * Task SCH-29. Design sections 13.10.6, 15.1 and 18.2.
 *
 * WHAT THE HUB IS FOR: it makes the DELIVERY POINT of an external protocol
 * frame deterministic. A frame always enters at a quantum boundary, in a fixed
 * endpoint order, carrying the frame index it entered at. It does not make the
 * ARRIVAL of a frame from a live editor deterministic, because a human moving
 * a knob is not deterministic; design section 13.5's purity claim is stated
 * over the recorded (frameIndex, frame) sequence and not over a session.
 *
 * THE HUB ALLOCATES ONCE, IN ITS CONSTRUCTOR, AND NEVER AGAIN. Design section
 * 13.10 rule 1 forbids allocation after construction, and toDevice is called
 * from an endpoint's own thread with a BORROWED buffer, so the hub must copy,
 * which means it must know at construction how big one frame can be and how
 * many can be queued. That is the whole reason both sizes are constructor
 * arguments. The total is
 *
 *     kMaxEndpoints x queueDepth x (sizeof(StampedFrame) + maxFrameBytes)
 *
 * and it is computable from this declaration alone, which is the property
 * SCH-29's check asserts.
 *
 * NEITHER SIZE HAS A STATUS CODE. The Board constructs this object before the
 * Scheduler exists, so Scheduler::create never sees either value; both are
 * design constants that no host, project file or patch can reach.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g2
{
	/* One G2 protocol message, framed as design section 15.3 describes.
	 *
	 * THE BUFFER IS ALWAYS BORROWED, AND THE LIFETIME DEPENDS ON THE
	 * DIRECTION. One rule for both directions is right going in and impossible
	 * coming out, so all three cases are spelled out rather than summarised --
	 * a borrowed buffer that someone stores is the most likely defect here.
	 *
	 *   INTO the hub -- toDevice() and fromDevice() arguments:
	 *       Valid only until the call returns. The hub COPIES what it keeps.
	 *
	 *   OUT of the hub -- drainToDevice()'s out-array:
	 *       Points into HUB-OWNED storage. Valid until the NEXT drainToDevice()
	 *       on the same hub, which is one quantum later. The caller must
	 *       consume or copy within the quantum.
	 *
	 *   OUT of the hub -- onFrameFromDevice()'s argument:
	 *       Points into the caller's storage. Valid only until the callback
	 *       returns; an endpoint that keeps it copies it. */
	struct ProtocolFrame
	{
		const uint8_t* data;
		size_t         size;
	};

	/* One protocol frame plus the quantum at which it entered. Declared HERE,
	 * before TransportHub, because drainToDevice takes StampedFrame* and fills
	 * an array of it; an earlier design draft placed this struct after the
	 * class that uses it. SCH-29's check reads this header and asserts the
	 * order, because no C++ expression can. */
	struct StampedFrame
	{
		uint64_t      frameIndex;   /* the quantum at which it entered */
		ProtocolFrame frame;
	};

	/* One attachment: the internal client, the forked G2-Edit socket, or the
	 * usbip adapter. Design section 15.1 lists them and fixes their order. */
	class TransportEndpoint
	{
	public:
		virtual ~TransportEndpoint() = default;

		/* Called on the scheduler thread, inside a quantum boundary. The
		 * implementation must not block and must not call back into the hub. */
		virtual void onFrameFromDevice(ProtocolFrame) noexcept = 0;
	};

	/* Ownership   The Board owns the hub. The hub does not own its endpoints;
	 *             each endpoint outlives the hub or detaches before it dies.
	 * Lifetime    Constructed with the Board, before any endpoint attaches.
	 * Threading   attach and detach are setup-time only and are not
	 *             thread-safe. fromDevice and drainToDevice are
	 *             scheduler-thread only. toDevice is the ONE method callable
	 *             from another thread, and it is single-producer for each
	 *             endpoint.
	 */
	class TransportHub
	{
	public:
		/* THE GENERATING COUNT FOR THE ALLOCATION FORMULA, AND THE REASON IT
		 * IS PUBLIC. Design section 15.1 fixes three attachments, and the
		 * hub's whole storage is reserved for all of them at construction --
		 * before any of them attaches. A consumer or a check that needed this
		 * factor and could not read it here would write the number 3 down
		 * instead, which is the shape this project has already paid for three
		 * times. */
		static constexpr size_t kMaxEndpoints = 3;

		/* THE TWO SIZES THE HUB CANNOT DERIVE FOR ITSELF.
		 *
		 *   maxFrameBytes  DERIVED from design section 15.3's wire framing,
		 *                  not measured: [1-byte type][2-byte length][payload]
		 *                  puts the ceiling at 1 + 2 + 65,535. It is the only
		 *                  bound the design can state without measuring real
		 *                  traffic and it is deliberately the loosest one.
		 *                  PROTO-10 measures the largest real patch message
		 *                  and lowers it; the value is a constructor argument
		 *                  for exactly that reason.
		 *   queueDepth     CHOSEN, not measured. An ordinary tunable, not a
		 *                  gated constant. */
		TransportHub(size_t maxFrameBytes, size_t queueDepth);

		/* Setup only, and not thread-safe. attach places the endpoint at the
		 * next free position, and that position is its ATTACHMENT ORDER --
		 * the order drainToDevice visits it in for the rest of the hub's life.
		 * detach frees the position and moves no other endpoint, so the
		 * relative order of the endpoints that stay is unchanged. */
		void attach(TransportEndpoint&);
		void detach(TransportEndpoint&);

		/* Device -> attachments. Scheduler thread, at a quantum boundary.
		 * Delivers to every attached endpoint in attachment order. */
		void fromDevice(ProtocolFrame) noexcept;

		/* Attachment -> device. Callable from the endpoint's own thread. It
		 * COPIES the borrowed buffer into that endpoint's own single-producer
		 * queue and returns false when the endpoint is not attached, when the
		 * frame is larger than maxFrameBytes, or when that endpoint's queue
		 * already holds queueDepth undrained frames. It never blocks and it
		 * never allocates.
		 *
		 * THE DEPTH AVAILABLE INSIDE ONE QUANTUM IS queueDepth MINUS WHAT THE
		 * LAST DRAIN LENT OUT, and that is a consequence of the borrow
		 * lifetime rather than a defect. The frames drainToDevice handed to
		 * the caller still occupy the slots a producer would refill, and they
		 * stay occupied until the next drain releases them. queueDepth is
		 * sized far above one quantum of editor traffic precisely so that this
		 * cost never binds in service; a producer that drives the full depth
		 * on every quantum will see it. */
		bool toDevice(TransportEndpoint&, ProtocolFrame) noexcept;

		/* Scheduler thread, once for each quantum. Drains every endpoint queue
		 * in a FIXED endpoint order -- attachment order -- and stamps each
		 * frame with the current frame index. The fixed order is what stops
		 * two attachments racing into a different interleaving on two runs.
		 *
		 * THE POINTERS IN `out` REFER TO HUB-OWNED STORAGE and stay valid
		 * until the NEXT drainToDevice on this hub. They do not die when this
		 * call returns; if they did, no caller could read them. That lifetime
		 * is why the slots a drain hands out are released at the START of the
		 * FOLLOWING drain and not at the end of this one.
		 *
		 * Returns the number of frames written to `out`, never more than
		 * `max`. Frames `max` left behind stay queued for the next drain.
		 *
		 * THE FRAME INDEX IS THIS HUB'S OWN QUANTUM ORDINAL: the first drain
		 * stamps 0, the next 1, and so on. The declared surface carries no way
		 * to inject the scheduler's index, and drainToDevice runs exactly once
		 * for each quantum, so the count of drains IS the quantum count. */
		size_t drainToDevice(StampedFrame* out, size_t max) noexcept;

	private:
		/* One endpoint's single-producer/single-consumer ring over the shared
		 * arenas. The three counters are MONOTONIC, never wrapped; the ring
		 * index is the counter modulo m_queueDepth.
		 *
		 *   m_release .. m_head   drained, still BORROWED by the caller
		 *   m_head    .. m_tail   queued, not yet drained
		 *
		 * The producer owns `tail` and reads `release`; the consumer owns
		 * `head` and `release` and reads `tail`. That is what makes toDevice
		 * callable from the endpoint's own thread without a lock. */
		struct Endpoint
		{
			TransportEndpoint*    handle = nullptr;
			std::atomic<uint64_t> tail{ 0 };
			std::atomic<uint64_t> head{ 0 };
			std::atomic<uint64_t> release{ 0 };
		};

		size_t indexOf(const TransportEndpoint&) const noexcept;

		const size_t m_maxFrameBytes;
		const size_t m_queueDepth;

		/* THE WHOLE ALLOCATION, AND IT HAPPENS IN THE CONSTRUCTOR'S MEMBER
		 * INITIALISER LIST. Two vectors, sized once:
		 *   m_stamped   kMaxEndpoints x queueDepth entries
		 *   m_payload   kMaxEndpoints x queueDepth x maxFrameBytes bytes
		 * Their byte totals sum to exactly the formula this header states. */
		std::vector<StampedFrame> m_stamped;
		std::vector<uint8_t>      m_payload;

		Endpoint m_endpoints[kMaxEndpoints];
		uint64_t m_frameIndex = 0;
	};
}
