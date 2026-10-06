#pragma once

#include "transportHub.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rg2
{
	// Plugin-side endpoint attached to TransportHub for message exchanges with device.
	class InternalClient final : public TransportEndpoint
	{
	public:
		InternalClient(TransportHub& _hub, size_t _maxFrameBytes, size_t _inboxDepth);
		~InternalClient() override;

		InternalClient(const InternalClient&) = delete;
		InternalClient& operator=(const InternalClient&) = delete;

		// Plugin thread -> device. Hands frame to hub. Non-blocking, non-allocating.
		bool send(ProtocolFrame _frame) noexcept;

		// Wraps message with 2-byte BE total prefix and 2-byte BE CRC-16/XMODEM and sends as single frame.
		bool sendTransfer(uint8_t* _buffer, std::size_t _messageSize) noexcept;

		// Device -> plugin. Called on scheduler thread; copies frame to inbox or drops if full.
		void onFrameFromDevice(ProtocolFrame _frame) noexcept override;

		// Drains oldest frame from inbox. Returned frame payload remains valid until next receive().
		bool receive(ProtocolFrame& _out) noexcept;

		// Total dropped frames due to size limits or inbox exhaustion.
		uint64_t droppedFrames() const noexcept;

	private:
		TransportHub& m_hub;

		const size_t m_maxFrameBytes;
		const size_t m_inboxDepth;

		std::vector<ProtocolFrame> m_frames;
		std::vector<uint8_t> m_payload;

		std::atomic<uint64_t> m_tail{0};
		std::atomic<uint64_t> m_head{0};
		std::atomic<uint64_t> m_release{0};

		std::atomic<uint64_t> m_dropped{0};
	};
} // namespace rg2
