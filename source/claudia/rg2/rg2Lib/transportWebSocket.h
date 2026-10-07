#pragma once

#include "transportHub.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rg2
{
	/* TransportWebSocketServer -- loopback WebSocket (RFC 6455) server endpoint attached to TransportHub.
	 *
	 * Bridges browser-based clients (such as the Nord Modular G2 Web Editor) to
	 * the G2 emulator. Automatically negotiates HTTP Upgrade to WebSocket,
	 * unmasks incoming client frames, and delivers frames to TransportHub::toDevice().
	 * Outgoing frames from onFrameFromDevice() are packaged into binary WebSocket frames.
	 */
	class TransportWebSocketServer final : public TransportEndpoint
	{
	public:
		TransportWebSocketServer(TransportHub& _hub, uint16_t _port = 7777);
		~TransportWebSocketServer() override;

		TransportWebSocketServer(const TransportWebSocketServer&) = delete;
		TransportWebSocketServer& operator=(const TransportWebSocketServer&) = delete;

		// Bind and listen on 127.0.0.1. A port of 0 selects an ephemeral port.
		// Probes up to +32 ports if the requested port is in use.
		bool listen();

		uint16_t port() const noexcept { return m_port; }

		// Accept a client connection. If _block is true, waits until a client connects.
		bool acceptClient(bool _block = false);

		bool hasClient() const noexcept;
		bool isUpgraded() const noexcept { return m_isUpgraded.load(std::memory_order_acquire); }

		void disconnectClient() noexcept;

		void close() noexcept;

		// TransportEndpoint callback: device -> websocket client.
		void onFrameFromDevice(ProtocolFrame _frame) noexcept override;

		// Reads from client socket, performs handshake or unpacks WS frames, forwards to hub.toDevice().
		size_t pumpSocket(int timeoutMs = 100) noexcept;

		void startBackgroundThread();
		void stopBackgroundThread() noexcept;
		bool isBackgroundThreadRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

		uint64_t framesIn() const noexcept { return m_framesIn.load(std::memory_order_relaxed); }
		uint64_t framesOut() const noexcept { return m_framesOut.load(std::memory_order_relaxed); }
		uint64_t droppedFrames() const noexcept { return m_dropped.load(std::memory_order_relaxed); }

		// Helpers for RFC 6455 handshake testing
		static std::string computeAcceptKey(const std::string& _secWebSocketKey);

		// Origin validation: allows connections without an Origin header (native desktop apps, DLL shims),
		// localhost/127.0.0.1, or explicitly registered origins. Untrusted origins are rejected with 403 Forbidden.
		bool isOriginAllowed(const std::string& _origin) const noexcept;
		void addAllowedOrigin(std::string _origin);

		// Optional shared token authentication. When set to non-empty, clients must supply the token
		// via query parameter (?token=...) or X-Auth-Token header. When empty (default), authentication is bypassed.
		void setAuthToken(std::string _token);
		const std::string& authToken() const noexcept { return m_authToken; }

	private:
		bool processHandshake() noexcept;
		size_t processFrames() noexcept;

		TransportHub& m_hub;
		uint16_t m_requestedPort;
		uint16_t m_port = 0;
		std::intptr_t m_listenFd = -1;
		std::intptr_t m_clientFd = -1;
		std::atomic<bool> m_hasClient{false};
		std::atomic<bool> m_isUpgraded{false};

		mutable std::mutex m_txMutex;
		std::vector<uint8_t> m_txBuffer;

		std::vector<uint8_t> m_rxBuffer;
		size_t m_rxOffset = 0;
		std::vector<std::string> m_allowedOrigins;
		std::string m_authToken;

		std::atomic<uint64_t> m_framesIn{0};
		std::atomic<uint64_t> m_framesOut{0};
		std::atomic<uint64_t> m_dropped{0};

		std::thread m_thread;
		std::atomic<bool> m_running{false};
	};
} // namespace rg2
