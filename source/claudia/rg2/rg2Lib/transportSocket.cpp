#include "transportSocket.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

#include <cerrno>
#include <chrono>
#include <cstring>

namespace rg2
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

	TransportSocketServer::TransportSocketServer(TransportHub& _hub, const uint16_t _port) :
		m_hub(_hub), m_requestedPort(_port)
	{
		m_hub.attach(*this);
	}

	TransportSocketServer::~TransportSocketServer()
	{
		m_hub.detach(*this);
		close();
	}

	bool TransportSocketServer::listen()
	{
		if (!socketsReady())
			return false;

		close();

		const PlatformSocket fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (fd == kInvalidSocket)
			return false;

		m_listenFd = static_cast<std::intptr_t>(fd);

		socketSetOption(m_listenFd, SOL_SOCKET, SO_REUSEADDR, 1);

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(m_requestedPort);

		if (::bind(socketArg(m_listenFd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
			::listen(socketArg(m_listenFd), 1) != 0)
		{
			socketClose(m_listenFd);
			return false;
		}

		sockaddr_in bound{};
#ifdef _WIN32
		int length = sizeof(bound);
#else
		socklen_t length = sizeof(bound);
#endif
		if (::getsockname(socketArg(m_listenFd), reinterpret_cast<sockaddr*>(&bound), &length) != 0)
		{
			socketClose(m_listenFd);
			return false;
		}

		m_port = ntohs(bound.sin_port);
		return true;
	}

	bool TransportSocketServer::acceptClient(const bool _block)
	{
		if (m_listenFd == -1)
			return false;

		if (!_block)
		{
#ifdef _WIN32
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(socketArg(m_listenFd), &fds);
			timeval tv{0, 0};
			if (::select(0, &fds, nullptr, nullptr, &tv) <= 0)
				return false;
#else
			pollfd pfd{};
			pfd.fd = int(m_listenFd);
			pfd.events = POLLIN;
			if (::poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0)
				return false;
#endif
		}

		const PlatformSocket client = ::accept(socketArg(m_listenFd), nullptr, nullptr);
		if (client == kInvalidSocket)
			return false;

		disconnectClient();

		m_clientFd = static_cast<std::intptr_t>(client);
		socketSetOption(m_clientFd, IPPROTO_TCP, TCP_NODELAY, 1);
		setNonBlocking(m_clientFd);
		m_rxBuffer.clear();
		return true;
	}

	bool TransportSocketServer::hasClient() const noexcept { return m_clientFd != -1; }

	void TransportSocketServer::disconnectClient() noexcept
	{
		socketClose(m_clientFd);
		m_rxBuffer.clear();
	}

	void TransportSocketServer::close() noexcept
	{
		disconnectClient();
		socketClose(m_listenFd);
		m_port = 0;
	}

	void TransportSocketServer::onFrameFromDevice(const ProtocolFrame _frame) noexcept
	{
		if (m_clientFd == -1 || _frame.data == nullptr)
			return;

		const uint16_t channel =
			(_frame.size == 16) ? SocketFrameHeader::kChannelNotification : SocketFrameHeader::kChannelBulk;

		const size_t totalFrameSize = sizeof(SocketFrameHeader) + _frame.size;
		std::vector<uint8_t> frameBuffer(totalFrameSize);
		writeBe32(frameBuffer.data() + 0, SocketFrameHeader::kMagic);
		writeBe16(frameBuffer.data() + 4, channel);
		writeBe16(frameBuffer.data() + 6, 0);
		writeBe32(frameBuffer.data() + 8, static_cast<uint32_t>(_frame.size));
		if (_frame.size > 0)
			std::memcpy(frameBuffer.data() + sizeof(SocketFrameHeader), _frame.data, _frame.size);

		if (!socketSendAll(m_clientFd, frameBuffer.data(), totalFrameSize))
		{
			disconnectClient();
			return;
		}

		++m_framesOut;
	}

	size_t TransportSocketServer::pumpSocket(const int timeoutMs) noexcept
	{
		if (m_clientFd == -1)
			return 0;

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

		size_t framesDelivered = 0;
		while (true)
		{
			uint8_t temp[4096];
			while (true)
			{
				const long n = socketRecv(m_clientFd, temp, sizeof(temp));
				if (n > 0)
				{
					m_rxBuffer.insert(m_rxBuffer.end(), temp, temp + n);
				}
				else
				{
					if (n == 0)
					{
						disconnectClient();
						return framesDelivered;
					}
					// n < 0 (EWOULDBLOCK / EAGAIN)
					break;
				}
			}

			while (m_rxBuffer.size() >= sizeof(SocketFrameHeader))
			{
				const uint32_t magic = readBe32(m_rxBuffer.data() + 0);
				if (magic != SocketFrameHeader::kMagic)
				{
					// Shift until magic found or buffer drained
					m_rxBuffer.erase(m_rxBuffer.begin());
					continue;
				}

				const uint32_t payloadLen = readBe32(m_rxBuffer.data() + 8);
				const size_t totalFrameSize = sizeof(SocketFrameHeader) + payloadLen;

				if (m_rxBuffer.size() < totalFrameSize)
					break;

				const uint8_t* const payloadPtr = m_rxBuffer.data() + sizeof(SocketFrameHeader);
				const bool accepted = m_hub.toDevice(*this, ProtocolFrame{payloadPtr, payloadLen});
				if (accepted)
				{
					++m_framesIn;
					++framesDelivered;
				}
				else
				{
					++m_dropped;
				}

				m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + totalFrameSize);
			}

			if (framesDelivered > 0 || timeoutMs <= 0)
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
			FD_SET(socketArg(m_clientFd), &fds);
			timeval tv{remainingMs / 1000, (remainingMs % 1000) * 1000};
			if (::select(0, &fds, nullptr, nullptr, &tv) <= 0)
				break;
#else
			pollfd pfd{};
			pfd.fd = int(m_clientFd);
			pfd.events = POLLIN;
			if (::poll(&pfd, 1, remainingMs) <= 0 || (pfd.revents & POLLIN) == 0)
				break;
#endif
		}

		return framesDelivered;
	}
} // namespace rg2
