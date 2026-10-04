#include "g2usb.h"
#include "../g2Lib/transportSocket.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace g2::wine
{
	namespace
	{
#ifdef _WIN32
		using PlatformSocket = SOCKET;
		constexpr PlatformSocket kInvalidSocket = INVALID_SOCKET;
#else
		using PlatformSocket = int;
		constexpr PlatformSocket kInvalidSocket = -1;
#endif

		PlatformSocket socketArg(const std::intptr_t _fd) noexcept { return static_cast<PlatformSocket>(_fd); }

		bool socketsReady() noexcept
		{
#ifdef _WIN32
			static const bool ready = []
			{
				WSADATA data{};
				return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
			}();
			return ready;
#else
			return true;
#endif
		}

		void socketClose(std::intptr_t& _fd) noexcept
		{
			if (_fd == -1)
				return;
#ifdef _WIN32
			::closesocket(socketArg(_fd));
#else
			::close(socketArg(_fd));
#endif
			_fd = -1;
		}

		int socketSetOption(const std::intptr_t _fd, const int _level, const int _option, const int _value) noexcept
		{
			const int value = _value;
#ifdef _WIN32
			return ::setsockopt(socketArg(_fd), _level, _option, reinterpret_cast<const char*>(&value),
								int(sizeof value));
#else
			return ::setsockopt(socketArg(_fd), _level, _option, reinterpret_cast<const char*>(&value),
								socklen_t(sizeof value));
#endif
		}

		void setNonBlocking(const std::intptr_t _fd) noexcept
		{
#ifdef _WIN32
			u_long mode = 1;
			::ioctlsocket(socketArg(_fd), FIONBIO, &mode);
#else
			const int flags = ::fcntl(socketArg(_fd), F_GETFL, 0);
			if (flags >= 0)
				::fcntl(socketArg(_fd), F_SETFL, flags | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
			socketSetOption(_fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
#endif
		}

		long socketSend(const std::intptr_t _fd, const void* _data, const size_t _size) noexcept
		{
#ifdef _WIN32
			return long(::send(socketArg(_fd), reinterpret_cast<const char*>(_data), int(_size), 0));
#else
			return long(::send(socketArg(_fd), _data, _size, 0));
#endif
		}

		bool socketSendAll(const std::intptr_t _fd, const uint8_t* const _data, const size_t _size) noexcept
		{
			size_t sent = 0;
			while (sent < _size)
			{
				const long n = socketSend(_fd, _data + sent, _size - sent);
				if (n > 0)
				{
					sent += static_cast<size_t>(n);
				}
				else
				{
#ifdef _WIN32
					const int err = ::WSAGetLastError();
					if (err == WSAEWOULDBLOCK)
					{
						fd_set fds;
						FD_ZERO(&fds);
						FD_SET(socketArg(_fd), &fds);
						timeval tv{0, 20000};
						if (::select(0, nullptr, &fds, nullptr, &tv) > 0)
							continue;
					}
#else
					if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
					{
						pollfd pfd{};
						pfd.fd = int(_fd);
						pfd.events = POLLOUT;
						if (::poll(&pfd, 1, 20) > 0)
							continue;
					}
#endif
					return false;
				}
			}
			return true;
		}

		long socketRecv(const std::intptr_t _fd, void* _data, const size_t _size) noexcept
		{
#ifdef _WIN32
			return long(::recv(socketArg(_fd), reinterpret_cast<char*>(_data), int(_size), 0));
#else
			return long(::recv(socketArg(_fd), _data, _size, 0));
#endif
		}

		uint32_t readBe32(const uint8_t* p) noexcept
		{
			return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
		}

		void writeBe32(uint8_t* p, const uint32_t v) noexcept
		{
			p[0] = static_cast<uint8_t>(v >> 24);
			p[1] = static_cast<uint8_t>(v >> 16);
			p[2] = static_cast<uint8_t>(v >> 8);
			p[3] = static_cast<uint8_t>(v);
		}

		uint16_t readBe16(const uint8_t* p) noexcept
		{
			return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
		}

		void writeBe16(uint8_t* p, const uint16_t v) noexcept
		{
			p[0] = static_cast<uint8_t>(v >> 8);
			p[1] = static_cast<uint8_t>(v);
		}
	} // namespace

	G2UsbDeviceSession::G2UsbDeviceSession()
	{
#ifdef _WIN32
		m_eventSignaler = [](uintptr_t h)
		{
			if (h != 0)
				::SetEvent(reinterpret_cast<HANDLE>(h));
		};
#endif
	}

	G2UsbDeviceSession::~G2UsbDeviceSession() { disconnect(); }

	bool G2UsbDeviceSession::connectToHub(const char* const host, const uint16_t port)
	{
		if (!socketsReady())
			return false;

		disconnect();

		const PlatformSocket fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (fd == kInvalidSocket)
			return false;

		m_sockFd = static_cast<std::intptr_t>(fd);

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
		{
			socketClose(m_sockFd);
			return false;
		}

		if (::connect(socketArg(m_sockFd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
		{
			socketClose(m_sockFd);
			return false;
		}

		const int nodelay = 1;
#ifdef _WIN32
		::setsockopt(socketArg(m_sockFd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
					 sizeof(nodelay));
#else
		::setsockopt(socketArg(m_sockFd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
					 socklen_t(sizeof(nodelay)));
#endif

		setNonBlocking(m_sockFd);
		return true;
	}

	void G2UsbDeviceSession::disconnect() noexcept
	{
		socketClose(m_sockFd);
		m_rxStream.clear();
	}

	bool G2UsbDeviceSession::isConnected() const noexcept { return m_sockFd != -1; }

	void G2UsbDeviceSession::setEventSignaler(EventSignaler signaler) { m_eventSignaler = std::move(signaler); }

	bool G2UsbDeviceSession::registerEvent(const uintptr_t eventHandle) noexcept
	{
		m_eventHandle = eventHandle;
		m_eventRegistered = true;

		if (!m_notificationQueue.empty() || !m_bulkInQueue.empty())
			notifyEventIfRegistered();

		return true;
	}

	bool G2UsbDeviceSession::unregisterEvent() noexcept
	{
		m_eventHandle = 0;
		m_eventRegistered = false;
		return true;
	}

	bool G2UsbDeviceSession::queryEvent(bool& registered) const noexcept
	{
		registered = m_eventRegistered;
		return true;
	}

	size_t G2UsbDeviceSession::dequeueNotification(uint8_t* const out16) noexcept
	{
		if (out16 == nullptr)
			return 0;

		if (m_notificationQueue.empty())
		{
			std::memset(out16, 0, 16);
			return 0;
		}

		const auto& rec = m_notificationQueue.front();
		std::memcpy(out16, rec.data(), 16);
		m_notificationQueue.pop_front();
		return 16;
	}

	bool G2UsbDeviceSession::writeFile(const uint8_t* const data, const size_t size, size_t& bytesWritten) noexcept
	{
		bytesWritten = 0;
		if (m_sockFd == -1 || data == nullptr)
			return false;

		const size_t totalSize = sizeof(SocketFrameHeader) + size;
		std::vector<uint8_t> frameBuffer(totalSize);
		writeBe32(frameBuffer.data() + 0, SocketFrameHeader::kMagic);
		writeBe16(frameBuffer.data() + 4, SocketFrameHeader::kChannelBulk);
		writeBe16(frameBuffer.data() + 6, 0);
		writeBe32(frameBuffer.data() + 8, static_cast<uint32_t>(size));
		if (size > 0)
			std::memcpy(frameBuffer.data() + sizeof(SocketFrameHeader), data, size);

		if (!socketSendAll(m_sockFd, frameBuffer.data(), totalSize))
		{
			disconnect();
			return false;
		}

		bytesWritten = size;
		return true;
	}

	bool G2UsbDeviceSession::readFile(uint8_t* const dst, const size_t maxBytes, size_t& bytesRead) noexcept
	{
		bytesRead = 0;
		if (dst == nullptr)
			return false;

		if (m_bulkInQueue.empty())
			pumpSocket();

		if (m_bulkInQueue.empty())
			return true;

		const size_t count = std::min(maxBytes, m_bulkInQueue.size());
		std::memcpy(dst, m_bulkInQueue.data(), count);
		m_bulkInQueue.erase(m_bulkInQueue.begin(), m_bulkInQueue.begin() + count);
		bytesRead = count;
		return true;
	}

	size_t G2UsbDeviceSession::pumpSocket(const int timeoutMs) noexcept
	{
		if (m_sockFd == -1)
			return 0;

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

		size_t packetsProcessed = 0;
		while (true)
		{
			uint8_t temp[4096];
			while (true)
			{
				const long n = socketRecv(m_sockFd, temp, sizeof(temp));
				if (n > 0)
				{
					m_rxStream.insert(m_rxStream.end(), temp, temp + n);
				}
				else
				{
					if (n == 0)
					{
						disconnect();
						return packetsProcessed;
					}
					break;
				}
			}

			while (m_rxStream.size() >= sizeof(SocketFrameHeader))
			{
				const uint32_t magic = readBe32(m_rxStream.data() + 0);
				if (magic != SocketFrameHeader::kMagic)
				{
					m_rxStream.erase(m_rxStream.begin());
					continue;
				}

				const uint16_t channel = readBe16(m_rxStream.data() + 4);
				const uint32_t payloadLen = readBe32(m_rxStream.data() + 8);
				const size_t totalSize = sizeof(SocketFrameHeader) + payloadLen;

				if (m_rxStream.size() < totalSize)
					break;

				const uint8_t* const payload = m_rxStream.data() + sizeof(SocketFrameHeader);

				if (channel == SocketFrameHeader::kChannelNotification && payloadLen == 16)
				{
					std::array<uint8_t, 16> rec{};
					std::memcpy(rec.data(), payload, 16);
					m_notificationQueue.push_back(rec);
					notifyEventIfRegistered();
					++packetsProcessed;
				}
				else if (channel == SocketFrameHeader::kChannelBulk)
				{
					if (payloadLen > 0)
					{
						m_bulkInQueue.insert(m_bulkInQueue.end(), payload, payload + payloadLen);
						notifyEventIfRegistered();
					}
					++packetsProcessed;
				}

				m_rxStream.erase(m_rxStream.begin(), m_rxStream.begin() + totalSize);
			}

			if (packetsProcessed > 0 || timeoutMs <= 0)
				break;

			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				break;

			const int remainingMs = static_cast<int>(
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
			if (remainingMs <= 0)
				break;

#ifdef _WIN32
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(socketArg(m_sockFd), &fds);
			timeval tv{remainingMs / 1000, (remainingMs % 1000) * 1000};
			if (::select(0, &fds, nullptr, nullptr, &tv) <= 0)
				break;
#else
			pollfd pfd{};
			pfd.fd = int(m_sockFd);
			pfd.events = POLLIN;
			if (::poll(&pfd, 1, remainingMs) <= 0 || (pfd.revents & POLLIN) == 0)
				break;
#endif
		}

		return packetsProcessed;
	}

	size_t G2UsbDeviceSession::notificationQueueDepth() const noexcept { return m_notificationQueue.size(); }

	size_t G2UsbDeviceSession::bulkInQueueDepth() const noexcept { return m_bulkInQueue.size(); }

	void G2UsbDeviceSession::notifyEventIfRegistered() noexcept
	{
		if (m_eventRegistered && m_eventSignaler)
		{
			m_eventSignaler(m_eventHandle);
		}
	}
} // namespace g2::wine
