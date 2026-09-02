/* g2::InternalClient, the plugin's own attachment to the transport hub.
 *
 * A peer of the usbip endpoint, not a path through it. Three attachments --
 * this client, the forked G2-Edit over a local socket, and the usbip adapter --
 * are siblings: each derives from TransportEndpoint and holds its own position
 * in the hub. That is why this class names no USB type, holds no other endpoint
 * and takes none as a constructor argument.
 *
 * The reason is concrete. To restore a DAW project the plugin must originate
 * protocol messages, and no editor is attached at that moment. A client that
 * routed the plugin's own messages through the USB stack would make project
 * restore depend on the USB stack being present, and would be impossible to
 * test with nothing else attached.
 *
 * It is the carriage for frames the plugin originates and for frames the device
 * sends back -- an attachment, and nothing more. It composes no message and
 * parses none. Nothing here computes a CRC, because nothing here builds a
 * message that would carry one.
 *
 * The two directions run on different threads.
 *
 *   send()        the plugin's thread. Hands a BORROWED frame to the hub,
 *                 which copies it. Returns the hub's verdict unchanged.
 *   onFrame-      the SCHEDULER thread, inside a quantum boundary. It must
 *   FromDevice()  not block and must not call back into the hub, so it copies
 *                 into a fixed inbox and returns; a frame that does not fit is
 *                 dropped and counted, never queued for later growth.
 *   receive()     the plugin's thread, draining that inbox.
 *
 * That makes the inbox a single-producer/single-consumer ring, and it is the
 * same shape TransportHub uses for an endpoint's outbound queue, with the same
 * borrow rule stated below.
 *
 * The client allocates once, in its constructor, and never again. Its whole
 * allocation is
 *
 *     inboxDepth x (sizeof(ProtocolFrame) + maxFrameBytes)
 *
 * and both factors are constructor arguments for the same reason the hub's
 * are: the producer is a real-time thread holding a borrowed buffer, so the
 * sizes cannot be derived after construction.
 */

#pragma once

#include "transportHub.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g2
{
	class InternalClient final : public TransportEndpoint
	{
	public:
		/* Attaches to the hub. attach is setup-time only and is not
		 * thread-safe, which is why it happens here and not on first use.
		 *
		 *   _maxFrameBytes  the largest DELIVERED frame the inbox can hold. It
		 *                   bounds this client's own storage only; the bound on
		 *                   what send() may originate is the hub's, and the hub
		 *                   reports it.
		 *   _inboxDepth     how many delivered frames may wait for the plugin
		 *                   to consume them. CHOSEN, not measured -- an
		 *                   ordinary tunable. */
		InternalClient(TransportHub& _hub, size_t _maxFrameBytes, size_t _inboxDepth);

		/* Detaches. A client that left its position occupied would silently
		 * cost one of the other two attachments its own:
		 * TransportHub::kMaxEndpoints reserves exactly three. */
		~InternalClient() override;

		InternalClient(const InternalClient&) = delete;
		InternalClient& operator=(const InternalClient&) = delete;

		/* Plugin thread -> device. The buffer is BORROWED and is valid only
		 * until this call returns; the hub copies what it keeps.
		 *
		 * Returns the hub's verdict unchanged, false when the frame is larger
		 * than the hub's maxFrameBytes or when this client's hub queue already
		 * holds undrained frames to its depth. A client that swallowed the
		 * refusal would tell the plugin that a patch message it never
		 * delivered was sent. It never blocks and it never allocates. */
		bool send(ProtocolFrame _frame) noexcept;

		/* Plugin thread -> device, at the TRANSFER level.
		 *
		 * The transfer envelope is the same shape as the message inside it --
		 * [2-byte BE total][body][2-byte BE CRC-16/XMODEM over body] -- with
		 * the message frame as the body, so a patch load carries its own total
		 * and CRC once at the message level and once again around the whole
		 * transfer. The total counts the WHOLE transfer including its own two
		 * prefix bytes, and the CRC sits DIRECTLY after the body: there is no
		 * pad, because the real wire terminates on the short last USB packet.
		 *
		 * The envelope is written in place and nothing is copied. `_buffer`
		 * holds the message at offset 2 and must have room for `_messageSize`
		 * plus four bytes; this call writes the two prefix bytes at the front
		 * and the two CRC bytes behind the message, then originates the whole
		 * of it as ONE frame.
		 *
		 * Returns the hub's verdict unchanged, and false without touching the
		 * buffer when the transfer would not fit a 16-bit total. */
		bool sendTransfer(uint8_t* _buffer, std::size_t _messageSize) noexcept;

		/* Device -> plugin. Scheduler thread, inside a quantum boundary. The
		 * argument is valid only until this call returns, so the frame is
		 * copied. A frame larger than _maxFrameBytes, or one arriving at a full
		 * inbox, is dropped whole and counted -- never truncated, and never
		 * written over a frame the plugin has not consumed. */
		void onFrameFromDevice(ProtocolFrame _frame) noexcept override;

		/* Plugin thread. Hands out the oldest undelivered frame and returns
		 * true, or returns false when the inbox is empty.
		 *
		 * What it hands out points into client-owned storage and stays valid
		 * until the next receive() on this client -- across further device
		 * deliveries, which is the case a naive ring overwrites. The slot is
		 * therefore released at the START of the following receive and not at
		 * the end of this one, and until then the producer may not refill it:
		 * a delivery that needs that slot is dropped, which is the visible
		 * price of a borrow the plugin can actually read. */
		bool receive(ProtocolFrame& _out) noexcept;

		/* How many delivered frames were dropped, for either reason. The
		 * callback cannot report to its caller -- the hub declares it void and
		 * the device is not listening -- so the count is where a drop becomes
		 * visible at all. */
		uint64_t droppedFrames() const noexcept;

	private:
		TransportHub& m_hub;

		const size_t m_maxFrameBytes;
		const size_t m_inboxDepth;

		/* The whole allocation, in the constructor's member initialiser list.
		 *   m_frames    _inboxDepth entries, each pointing into m_payload
		 *   m_payload   _inboxDepth x _maxFrameBytes bytes */
		std::vector<ProtocolFrame> m_frames;
		std::vector<uint8_t>       m_payload;

		/* The three counters are monotonic, never wrapped; the ring index is
		 * the counter modulo m_inboxDepth.
		 *
		 *   m_release .. m_head   received, still borrowed by the plugin
		 *   m_head    .. m_tail   queued, not yet received
		 *
		 * The producer owns `tail` and reads `release`; the consumer owns
		 * `head` and `release` and reads `tail`. */
		std::atomic<uint64_t> m_tail{ 0 };
		std::atomic<uint64_t> m_head{ 0 };
		std::atomic<uint64_t> m_release{ 0 };

		std::atomic<uint64_t> m_dropped{ 0 };
	};
}
