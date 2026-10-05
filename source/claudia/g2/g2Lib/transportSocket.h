#pragma once

#include "transportHub.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g2
{
#pragma pack(push, 1)
	struct SocketFrameHeader
	{
		static constexpr uint32_t kMagic = 0x47325553; // 'G2US'
		static constexpr uint16_t kChannelBulk = 0;
		static constexpr uint16_t kChannelNotification = 1;

		uint32_t magic;
		uint16_t channel;
		uint16_t reserved;
		uint32_t payloadLen;
	};
#pragma pack(pop)

	/* TransportSocketServer -- loopback TCP socket endpoint attached to TransportHub.
	 *
	 * Bridges an external client (such as the Wine user-mode proxy DLL) to the
	 * G2 emulator. Frames from the client socket are decoded and forwarded to
	 * TransportHub::toDevice(). Frames from the device arriving via
	 * onFrameFromDevice() are encoded and sent out over the socket.
	 */
	class TransportSocketServer final : public TransportEndpoint
	{
	public:
		TransportSocketServer(TransportHub& _hub, uint16_t _port = 7777);
		~TransportSocketServer() override;

		TransportSocketServer(const TransportSocketServer&) = delete;
		TransportSocketServer& operator=(const TransportSocketServer&) = delete;

		// Bind and listen on 127.0.0.1. A port of 0 asks the OS for an ephemeral port.
		bool listen();

		uint16_t port() const noexcept { return m_port; }

		// Accept a client connection. If _block is true, waits until a client connects.
		bool acceptClient(bool _block = false);

		bool hasClient() const noexcept;

		void disconnectClient() noexcept;

		void close() noexcept;

		// TransportEndpoint callback: device -> socket client.
		void onFrameFromDevice(ProtocolFrame _frame) noexcept override;

		// Reads from client socket, reassembles frames, forwards to hub.toDevice().
		size_t pumpSocket(int timeoutMs = 100) noexcept;

		uint64_t framesIn() const noexcept { return m_framesIn.load(std::memory_order_relaxed); }
		uint64_t framesOut() const noexcept { return m_framesOut.load(std::memory_order_relaxed); }
		uint64_t droppedFrames() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

	private:
		TransportHub& m_hub;
		uint16_t m_requestedPort;
		uint16_t m_port = 0;
		std::intptr_t m_listenFd = -1;
		std::intptr_t m_clientFd = -1;
		std::atomic<bool> m_hasClient{false};

		std::vector<uint8_t> m_rxBuffer;

		std::atomic<uint64_t> m_framesIn{0};
		std::atomic<uint64_t> m_framesOut{0};
		std::atomic<uint64_t> m_dropped{0};
	};
} // namespace g2
