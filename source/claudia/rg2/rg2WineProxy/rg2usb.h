#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace rg2::wine
{
	constexpr uint32_t kIoctlRegisterEvent = 0x222000u;
	constexpr uint32_t kIoctlUnregisterEvent = 0x222004u;
	constexpr uint32_t kIoctlQueryEvent = 0x222008u;
	constexpr uint32_t kIoctlGetNotification = 0x22200Cu;

	constexpr const char* kClaviaDeviceGuidString = "{CB3ED981-6125-4047-BC2A-292E370CC89A}";

	constexpr const char* kVirtualDevicePath =
		"\\\\?\\usb#vid_0ffc&pid_0002#clavia_g2_virtual#{cb3ed981-6125-4047-bc2a-292e370cc89a}";

	/* G2UsbDeviceSession -- user-mode proxy session bridging the Windows editor
	 * to the G2 TransportHub via a loopback TCP socket.
	 *
	 * Intercepts Clavia GUID enumeration, handles the 3 private IOCTLs in user
	 * mode, streams bulk OUT and IN over TCP, and signals the editor's event
	 * handle when incoming data or notifications arrive.
	 */
	class G2UsbDeviceSession
	{
	public:
		using EventSignaler = std::function<void(uintptr_t)>;

		G2UsbDeviceSession();
		~G2UsbDeviceSession();

		G2UsbDeviceSession(const G2UsbDeviceSession&) = delete;
		G2UsbDeviceSession& operator=(const G2UsbDeviceSession&) = delete;

		// Connects to TransportSocketServer over TCP loopback.
		bool connectToHub(const char* host = "127.0.0.1", uint16_t port = 7777);
		void disconnect() noexcept;
		bool isConnected() const noexcept;

		void setEventSignaler(EventSignaler signaler);

		// IOCTL 0x222000: Register unnamed completion event handle.
		bool registerEvent(uintptr_t eventHandle) noexcept;
		// IOCTL 0x222004: Unregister completion event handle.
		bool unregisterEvent() noexcept;
		// IOCTL 0x222008: Query whether an event is registered.
		bool queryEvent(bool& registered) const noexcept;
		// IOCTL 0x22200C: Dequeue one 16-byte notification record from ring.
		size_t dequeueNotification(uint8_t* out16) noexcept;

		// Bulk OUT (WriteFile).
		bool writeFile(const uint8_t* data, size_t size, size_t& bytesWritten) noexcept;
		// Bulk IN (ReadFile).
		bool readFile(uint8_t* dst, size_t maxBytes, size_t& bytesRead) noexcept;

		// Processes incoming TCP stream bytes into notification and bulk queues.
		size_t pumpSocket(int timeoutMs = 100) noexcept;

		size_t notificationQueueDepth() const noexcept;
		size_t bulkInQueueDepth() const noexcept;

	private:
		void notifyEventIfRegistered() noexcept;

		std::intptr_t m_sockFd = -1;
		uintptr_t m_eventHandle = 0;
		bool m_eventRegistered = false;
		EventSignaler m_eventSignaler;

		std::vector<uint8_t> m_rxStream;
		std::deque<std::array<uint8_t, 16>> m_notificationQueue;
		std::vector<uint8_t> m_bulkInQueue;
	};
} // namespace rg2::wine
