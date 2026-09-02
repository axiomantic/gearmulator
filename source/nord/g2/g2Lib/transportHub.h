/* g2::TransportHub and the two frame types it moves.
 *
 * The hub makes the delivery point of an external protocol frame
 * deterministic. A frame always enters at a quantum boundary, in a fixed
 * endpoint order, carrying the frame index it entered at. It does not make the
 * arrival of a frame from a live editor deterministic, because a human moving
 * a knob is not deterministic.
 *
 * The hub allocates once, in its constructor, and never again. toDevice is
 * called from an endpoint's own thread with a borrowed buffer, so the hub must
 * copy, which means it must know at construction how big one frame can be and
 * how many can be queued. That is the whole reason both sizes are constructor
 * arguments. The total is
 *
 *     kMaxEndpoints x queueDepth x (sizeof(StampedFrame) + maxFrameBytes)
 *
 * Neither size has a status code: both are design constants that no host,
 * project file or patch can reach.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g2
{
	/* One G2 protocol message.
	 *
	 * The buffer is always borrowed, and the lifetime depends on the direction.
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

	/* One protocol frame plus the quantum at which it entered. Declared here,
	 * before TransportHub, because drainToDevice takes StampedFrame* and fills
	 * an array of it. */
	struct StampedFrame
	{
		uint64_t      frameIndex;   /* the quantum at which it entered */
		ProtocolFrame frame;
	};

	/* One attachment: the internal client, the forked G2-Edit socket, or the
	 * usbip adapter. */
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
		/* The generating count for the allocation formula. The hub's whole
		 * storage is reserved for all attachments at construction, before any
		 * of them attaches. It is public so that a consumer needing this
		 * factor reads it here instead of writing the number down. */
		static constexpr size_t kMaxEndpoints = 3;

		/* The two sizes the hub cannot derive for itself.
		 *
		 *   maxFrameBytes  Derived from the wire framing, not measured:
		 *                  [1-byte type][2-byte length][payload] puts the
		 *                  ceiling at 1 + 2 + 65,535. It is deliberately the
		 *                  loosest bound; measuring real traffic lowers it,
		 *                  which is why the value is a constructor argument.
		 *   queueDepth     Chosen, not measured. An ordinary tunable. */
		TransportHub(size_t maxFrameBytes, size_t queueDepth);

		/* Setup only, and not thread-safe. attach places the endpoint at the
		 * next free position, and that position is its attachment order --
		 * the order drainToDevice visits it in for the rest of the hub's life.
		 * detach frees the position and moves no other endpoint, so the
		 * relative order of the endpoints that stay is unchanged. */
		void attach(TransportEndpoint&);
		void detach(TransportEndpoint&);

		/* Device -> attachments. Scheduler thread, at a quantum boundary.
		 * Delivers to every attached endpoint in attachment order. */
		void fromDevice(ProtocolFrame) noexcept;

		/* Attachment -> device. Callable from the endpoint's own thread. It
		 * copies the borrowed buffer into that endpoint's own single-producer
		 * queue and returns false when the endpoint is not attached, when the
		 * frame is larger than maxFrameBytes, or when that endpoint's queue
		 * already holds queueDepth undrained frames. It never blocks and it
		 * never allocates.
		 *
		 * The depth available inside one quantum is queueDepth minus what the
		 * last drain lent out: the frames drainToDevice handed to the caller
		 * still occupy the slots a producer would refill, and they stay
		 * occupied until the next drain releases them. queueDepth is sized far
		 * above one quantum of editor traffic so that this cost never binds in
		 * service; a producer that drives the full depth on every quantum will
		 * see it. */
		bool toDevice(TransportEndpoint&, ProtocolFrame) noexcept;

		/* Scheduler thread, once for each quantum. Drains every endpoint queue
		 * in a FIXED endpoint order -- attachment order -- and stamps each
		 * frame with the current frame index. The fixed order is what stops
		 * two attachments racing into a different interleaving on two runs.
		 *
		 * The pointers in `out` refer to hub-owned storage and stay valid
		 * until the next drainToDevice on this hub. That lifetime is why the
		 * slots a drain hands out are released at the start of the following
		 * drain and not at the end of this one.
		 *
		 * Returns the number of frames written to `out`, never more than
		 * `max`. Frames `max` left behind stay queued for the next drain.
		 *
		 * The frame index is this hub's own quantum ordinal: the first drain
		 * stamps 0, the next 1, and so on. drainToDevice runs exactly once for
		 * each quantum, so the count of drains is the quantum count. */
		size_t drainToDevice(StampedFrame* out, size_t max) noexcept;

	private:
		/* One endpoint's single-producer/single-consumer ring over the shared
		 * arenas. The three counters are monotonic, never wrapped; the ring
		 * index is the counter modulo m_queueDepth.
		 *
		 *   m_release .. m_head   drained, still borrowed by the caller
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

		/* The whole allocation, in the constructor's member initialiser list.
		 * Two vectors, sized once:
		 *   m_stamped   kMaxEndpoints x queueDepth entries
		 *   m_payload   kMaxEndpoints x queueDepth x maxFrameBytes bytes */
		std::vector<StampedFrame> m_stamped;
		std::vector<uint8_t>      m_payload;

		Endpoint m_endpoints[kMaxEndpoints];
		uint64_t m_frameIndex = 0;
	};
}
