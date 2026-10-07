#include "transportWebSocket.h"

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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

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

		inline uint32_t rol32(const uint32_t val, const int bits) noexcept
		{
			return (val << bits) | (val >> (32 - bits));
		}

		void sha1(const uint8_t* const data, const size_t len, uint8_t digest[20]) noexcept
		{
			uint32_t h0 = 0x67452301u;
			uint32_t h1 = 0xEFCDAB89u;
			uint32_t h2 = 0x98BADCFEu;
			uint32_t h3 = 0x10325476u;
			uint32_t h4 = 0xC3D2E1F0u;

			const size_t bitLen = len * 8u;
			const size_t padLen = (len % 64u < 56u) ? (56u - (len % 64u)) : (120u - (len % 64u));
			const size_t totalLen = len + padLen + 8u;

			std::vector<uint8_t> padded(totalLen, 0);
			if (len > 0)
				std::memcpy(padded.data(), data, len);
			padded[len] = 0x80u;

			for (int i = 0; i < 8; ++i)
				padded[totalLen - 1u - i] = static_cast<uint8_t>((bitLen >> (i * 8u)) & 0xFFu);

			for (size_t chunk = 0; chunk < totalLen; chunk += 64u)
			{
				uint32_t w[80];
				for (int i = 0; i < 16; ++i)
				{
					const uint8_t* const p = padded.data() + chunk + i * 4;
					w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
				}
				for (int i = 16; i < 80; ++i)
					w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

				uint32_t a = h0;
				uint32_t b = h1;
				uint32_t c = h2;
				uint32_t d = h3;
				uint32_t e = h4;

				for (int i = 0; i < 80; ++i)
				{
					uint32_t f, k;
					if (i < 20)
					{
						f = (b & c) | ((~b) & d);
						k = 0x5A827999u;
					}
					else if (i < 40)
					{
						f = b ^ c ^ d;
						k = 0x6ED9EBA1u;
					}
					else if (i < 60)
					{
						f = (b & c) | (b & d) | (c & d);
						k = 0x8F1BBCDCu;
					}
					else
					{
						f = b ^ c ^ d;
						k = 0xCA62C1D6u;
					}

					const uint32_t temp = rol32(a, 5) + f + e + k + w[i];
					e = d;
					d = c;
					c = rol32(b, 30);
					b = a;
					a = temp;
				}

				h0 += a;
				h1 += b;
				h2 += c;
				h3 += d;
				h4 += e;
			}

			const uint32_t h[5] = {h0, h1, h2, h3, h4};
			for (int i = 0; i < 5; ++i)
			{
				digest[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFFu);
				digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
				digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFFu);
				digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFFu);
			}
		}

		std::string base64Encode(const uint8_t* const data, const size_t len)
		{
			static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string out;
			out.reserve(((len + 2) / 3) * 4);
			for (size_t i = 0; i < len; i += 3)
			{
				const uint32_t b0 = data[i];
				const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
				const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
				const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

				out.push_back(table[(triple >> 18) & 0x3F]);
				out.push_back(table[(triple >> 12) & 0x3F]);
				out.push_back((i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=');
				out.push_back((i + 2 < len) ? table[triple & 0x3F] : '=');
			}
			return out;
		}

	} // namespace

	std::string TransportWebSocketServer::computeAcceptKey(const std::string& _secWebSocketKey)
	{
		const std::string combined = _secWebSocketKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		uint8_t digest[20];
		sha1(reinterpret_cast<const uint8_t*>(combined.data()), combined.size(), digest);
		return base64Encode(digest, 20);
	}

	TransportWebSocketServer::TransportWebSocketServer(TransportHub& _hub, const uint16_t _port) :
		m_hub(_hub), m_requestedPort(_port)
	{
		socketsReady();
		m_allowedOrigins.push_back("http://localhost");
		m_allowedOrigins.push_back("https://localhost");
		m_allowedOrigins.push_back("http://127.0.0.1");
		m_allowedOrigins.push_back("https://127.0.0.1");
		m_allowedOrigins.push_back("https://nordmodulareditor.com");
		m_allowedOrigins.push_back("http://nordmodulareditor.com");
		m_hub.attach(*this);
	}

	TransportWebSocketServer::~TransportWebSocketServer()
	{
		m_hub.detach(*this);
		close();
	}

	bool TransportWebSocketServer::isOriginAllowed(const std::string& _origin) const noexcept
	{
		if (_origin.empty() || _origin == "null")
			return true;

		std::string lowerOrigin = _origin;
		std::transform(lowerOrigin.begin(), lowerOrigin.end(), lowerOrigin.begin(),
					   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		for (const auto& allowed : m_allowedOrigins)
		{
			std::string lowerAllowed = allowed;
			std::transform(lowerAllowed.begin(), lowerAllowed.end(), lowerAllowed.begin(),
						   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			if (lowerOrigin == lowerAllowed)
				return true;

			// Match origin with arbitrary port (e.g. http://localhost:7777 matching http://localhost)
			if (lowerOrigin.rfind(lowerAllowed + ":", 0) == 0)
				return true;
		}
		return false;
	}

	void TransportWebSocketServer::addAllowedOrigin(std::string _origin)
	{
		if (!_origin.empty())
			m_allowedOrigins.push_back(std::move(_origin));
	}

	void TransportWebSocketServer::setAuthToken(std::string _token) { m_authToken = std::move(_token); }

	bool TransportWebSocketServer::listen()
	{
		close();

		const uint16_t startPort = m_requestedPort;
		const uint16_t maxAttempts = (startPort == 0) ? 1 : 33;

		for (uint16_t attempt = 0; attempt < maxAttempts; ++attempt)
		{
			const uint16_t tryPort = (startPort == 0) ? 0 : static_cast<uint16_t>(startPort + attempt);

			const PlatformSocket listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listenSock == kInvalidSocket)
				return false;

			m_listenFd = static_cast<std::intptr_t>(listenSock);
			socketSetOption(m_listenFd, SOL_SOCKET, SO_REUSEADDR, 1);
			setNonBlocking(m_listenFd);

			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(tryPort);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			if (::bind(socketArg(m_listenFd), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0)
			{
				if (::listen(socketArg(m_listenFd), 2) == 0)
				{
					sockaddr_in boundAddr{};
					socklen_t boundLen = sizeof(boundAddr);
					if (::getsockname(socketArg(m_listenFd), reinterpret_cast<sockaddr*>(&boundAddr), &boundLen) == 0)
					{
						m_port = ntohs(boundAddr.sin_port);
					}
					else
					{
						m_port = tryPort;
					}
					return true;
				}
			}

			socketClose(m_listenFd);
		}

		close();
		return false;
	}

	bool TransportWebSocketServer::acceptClient(const bool _block)
	{
		if (m_listenFd == -1)
			return false;

		if (_block)
		{
#ifdef _WIN32
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(socketArg(m_listenFd), &fds);
			timeval tv{5, 0};
			if (::select(0, &fds, nullptr, nullptr, &tv) <= 0)
				return false;
#else
			pollfd pfd{};
			pfd.fd = int(m_listenFd);
			pfd.events = POLLIN;
			if (::poll(&pfd, 1, 5000) <= 0 || (pfd.revents & POLLIN) == 0)
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
		m_hasClient.store(true, std::memory_order_release);
		m_isUpgraded.store(false, std::memory_order_release);
		return true;
	}

	bool TransportWebSocketServer::hasClient() const noexcept { return m_hasClient.load(std::memory_order_acquire); }

	void TransportWebSocketServer::disconnectClient() noexcept
	{
		m_hasClient.store(false, std::memory_order_release);
		m_isUpgraded.store(false, std::memory_order_release);
		socketClose(m_clientFd);
		m_rxBuffer.clear();
	}

	void TransportWebSocketServer::close() noexcept
	{
		stopBackgroundThread();
		disconnectClient();
		socketClose(m_listenFd);
		m_port = 0;
	}

	void TransportWebSocketServer::onFrameFromDevice(const ProtocolFrame _frame) noexcept
	{
		if (m_clientFd == -1 || !m_isUpgraded.load(std::memory_order_acquire) || _frame.data == nullptr)
			return;

		const uint8_t channel = (_frame.size == 16) ? 1 : 0;
		const size_t totalPayload = 1 + _frame.size;

		std::vector<uint8_t> frameBuffer;
		if (totalPayload < 126)
		{
			frameBuffer.resize(2 + totalPayload);
			frameBuffer[0] = 0x82u; // FIN | binary
			frameBuffer[1] = static_cast<uint8_t>(totalPayload);
			frameBuffer[2] = channel;
			if (_frame.size > 0)
				std::memcpy(frameBuffer.data() + 3, _frame.data, _frame.size);
		}
		else if (totalPayload <= 0xFFFFu)
		{
			frameBuffer.resize(4 + totalPayload);
			frameBuffer[0] = 0x82u;
			frameBuffer[1] = 126u;
			frameBuffer[2] = static_cast<uint8_t>((totalPayload >> 8) & 0xFFu);
			frameBuffer[3] = static_cast<uint8_t>(totalPayload & 0xFFu);
			frameBuffer[4] = channel;
			if (_frame.size > 0)
				std::memcpy(frameBuffer.data() + 5, _frame.data, _frame.size);
		}
		else
		{
			// Unsupported single-frame size above 64 KB
			++m_dropped;
			return;
		}

		if (!socketSendAll(m_clientFd, frameBuffer.data(), frameBuffer.size()))
		{
			disconnectClient();
			return;
		}

		++m_framesOut;
	}

	bool TransportWebSocketServer::processHandshake() noexcept
	{
		// Look for end of HTTP header
		std::string req(reinterpret_cast<const char*>(m_rxBuffer.data()), m_rxBuffer.size());
		const size_t headerEnd = req.find("\r\n\r\n");
		if (headerEnd == std::string::npos)
		{
			if (m_rxBuffer.size() > 8192)
			{
				disconnectClient();
				return false;
			}
			return false; // Wait for more data
		}

		std::string lowerReq = req.substr(0, headerEnd);
		std::transform(lowerReq.begin(), lowerReq.end(), lowerReq.begin(),
					   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		if (lowerReq.find("upgrade") != std::string::npos && lowerReq.find("websocket") != std::string::npos)
		{
			// Extract Origin header (if present)
			std::string origin;
			const std::string originHeader = "origin:";
			const size_t originPos = lowerReq.find(originHeader);
			if (originPos != std::string::npos)
			{
				size_t valStart = originPos + originHeader.size();
				while (valStart < headerEnd && (req[valStart] == ' ' || req[valStart] == '\t'))
					++valStart;
				size_t valEnd = valStart;
				while (valEnd < headerEnd && req[valEnd] != '\r' && req[valEnd] != '\n')
					++valEnd;
				origin = req.substr(valStart, valEnd - valStart);
			}

			// Validate Origin
			if (!isOriginAllowed(origin))
			{
				const std::string forbidden = "HTTP/1.1 403 Forbidden\r\n"
											  "Content-Type: text/plain\r\n"
											  "Connection: close\r\n\r\n"
											  "Forbidden Origin\r\n";
				socketSendAll(m_clientFd, reinterpret_cast<const uint8_t*>(forbidden.data()), forbidden.size());
				disconnectClient();
				return false;
			}

			// If authToken is set, validate token from query parameter (?token=...) or X-Auth-Token header
			if (!m_authToken.empty())
			{
				bool tokenValid = false;
				const size_t firstLineEnd = req.find("\r\n");
				if (firstLineEnd != std::string::npos)
				{
					const std::string firstLine = req.substr(0, firstLineEnd);
					const std::string tokenParam = "token=" + m_authToken;
					const auto tPos = firstLine.find(tokenParam);
					if (tPos != std::string::npos)
					{
						const size_t nextChar = tPos + tokenParam.size();
						if (nextChar >= firstLine.size() || firstLine[nextChar] == '&' || firstLine[nextChar] == ' ' ||
							firstLine[nextChar] == '#')
							tokenValid = true;
					}
				}

				if (!tokenValid)
				{
					const std::string authHeader = "x-auth-token:";
					const size_t authPos = lowerReq.find(authHeader);
					if (authPos != std::string::npos)
					{
						size_t valStart = authPos + authHeader.size();
						while (valStart < headerEnd && (req[valStart] == ' ' || req[valStart] == '\t'))
							++valStart;
						size_t valEnd = valStart;
						while (valEnd < headerEnd && req[valEnd] != '\r' && req[valEnd] != '\n')
							++valEnd;
						if (req.substr(valStart, valEnd - valStart) == m_authToken)
							tokenValid = true;
					}
				}

				if (!tokenValid)
				{
					const std::string unauthorized = "HTTP/1.1 401 Unauthorized\r\n"
													 "Content-Type: text/plain\r\n"
													 "Connection: close\r\n\r\n"
													 "Unauthorized\r\n";
					socketSendAll(m_clientFd, reinterpret_cast<const uint8_t*>(unauthorized.data()),
								  unauthorized.size());
					disconnectClient();
					return false;
				}
			}

			// Search for Sec-WebSocket-Key
			const std::string keyHeader = "sec-websocket-key:";
			const size_t keyPos = lowerReq.find(keyHeader);
			if (keyPos == std::string::npos)
			{
				disconnectClient();
				return false;
			}

			size_t valStart = keyPos + keyHeader.size();
			while (valStart < headerEnd && (req[valStart] == ' ' || req[valStart] == '\t'))
				++valStart;

			size_t valEnd = valStart;
			while (valEnd < headerEnd && req[valEnd] != '\r' && req[valEnd] != '\n')
				++valEnd;

			const std::string secKey = req.substr(valStart, valEnd - valStart);
			const std::string acceptKey = computeAcceptKey(secKey);

			std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
								   "Upgrade: websocket\r\n"
								   "Connection: Upgrade\r\n"
								   "Sec-WebSocket-Accept: " +
				acceptKey +
				"\r\n"
				"Access-Control-Allow-Private-Network: true\r\n";

			if (!origin.empty() && origin != "null")
			{
				response += "Access-Control-Allow-Origin: " + origin + "\r\n";
			}

			response += "\r\n";

			if (!socketSendAll(m_clientFd, reinterpret_cast<const uint8_t*>(response.data()), response.size()))
			{
				disconnectClient();
				return false;
			}

			m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + headerEnd + 4);
			m_isUpgraded.store(true, std::memory_order_release);
			return true;
		}

		// Non-WebSocket requests: reject with HTTP 400 Bad Request
		const std::string badRequest = "HTTP/1.1 400 Bad Request\r\n"
									   "Sec-WebSocket-Version: 13\r\n"
									   "Connection: close\r\n\r\n";
		socketSendAll(m_clientFd, reinterpret_cast<const uint8_t*>(badRequest.data()), badRequest.size());
		disconnectClient();
		return false;
	}

	size_t TransportWebSocketServer::processFrames() noexcept
	{
		size_t delivered = 0;
		while (m_rxBuffer.size() >= 2)
		{
			const uint8_t b0 = m_rxBuffer[0];
			const uint8_t b1 = m_rxBuffer[1];

			const uint8_t opcode = b0 & 0x0Fu;
			const bool isMasked = (b1 & 0x80u) != 0;
			const uint8_t len7 = b1 & 0x7Fu;

			size_t hdrLen = 2;
			uint64_t payloadLen = len7;

			if (len7 == 126)
			{
				if (m_rxBuffer.size() < hdrLen + 2)
					break;
				payloadLen = (uint64_t(m_rxBuffer[2]) << 8) | uint64_t(m_rxBuffer[3]);
				hdrLen += 2;
			}
			else if (len7 == 127)
			{
				if (m_rxBuffer.size() < hdrLen + 8)
					break;
				payloadLen = 0;
				for (int i = 0; i < 8; ++i)
					payloadLen = (payloadLen << 8) | uint64_t(m_rxBuffer[hdrLen + i]);
				hdrLen += 8;
			}

			const uint8_t* maskKey = nullptr;
			if (isMasked)
			{
				if (m_rxBuffer.size() < hdrLen + 4)
					break;
				maskKey = m_rxBuffer.data() + hdrLen;
				hdrLen += 4;
			}

			const size_t totalFrameSize = hdrLen + static_cast<size_t>(payloadLen);
			if (m_rxBuffer.size() < totalFrameSize)
				break;

			uint8_t* const payload = m_rxBuffer.data() + hdrLen;
			if (isMasked && maskKey)
			{
				for (size_t i = 0; i < payloadLen; ++i)
					payload[i] ^= maskKey[i % 4];
			}

			if (opcode == 0x08) // Close
			{
				const uint8_t closeReply[2] = {0x88, 0x00};
				socketSendAll(m_clientFd, closeReply, sizeof(closeReply));
				disconnectClient();
				return delivered;
			}
			else if (opcode == 0x09) // Ping -> reply Pong
			{
				std::vector<uint8_t> pong(2 + payloadLen);
				pong[0] = 0x8A;
				pong[1] = static_cast<uint8_t>(payloadLen & 0x7Fu);
				if (payloadLen > 0)
					std::memcpy(pong.data() + 2, payload, static_cast<size_t>(payloadLen));
				socketSendAll(m_clientFd, pong.data(), pong.size());
			}
			else if (opcode == 0x02 || opcode == 0x00) // Binary
			{
				if (payloadLen > 0)
				{
					// If first byte is channel prefix (0x00 or 0x01) and payload > 1, pass payload without channel tag
					const uint8_t* deliverPtr = payload;
					size_t deliverSize = static_cast<size_t>(payloadLen);
					if (payloadLen > 1 && (payload[0] == 0x00 || payload[0] == 0x01))
					{
						deliverPtr = payload + 1;
						deliverSize = static_cast<size_t>(payloadLen - 1);
					}

					const bool accepted = m_hub.toDevice(*this, ProtocolFrame{deliverPtr, deliverSize});
					if (accepted)
					{
						++m_framesIn;
						++delivered;
					}
					else
					{
						++m_dropped;
					}
				}
			}

			m_rxBuffer.erase(m_rxBuffer.begin(), m_rxBuffer.begin() + totalFrameSize);
		}

		return delivered;
	}

	size_t TransportWebSocketServer::pumpSocket(const int timeoutMs) noexcept
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
					break;
				}
			}

			if (!m_isUpgraded.load(std::memory_order_acquire))
			{
				if (processHandshake())
					framesDelivered += processFrames();
			}
			else
			{
				framesDelivered += processFrames();
			}

			if (framesDelivered > 0 || timeoutMs <= 0)
				break;

			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				break;

			const int remainingMs =
				static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
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

	void TransportWebSocketServer::startBackgroundThread()
	{
		if (m_thread.joinable())
			return;

		m_running.store(true, std::memory_order_release);
		m_thread = std::thread(
			[this]
			{
				while (m_running.load(std::memory_order_acquire))
				{
					if (!hasClient())
					{
						acceptClient(false);
					}

					if (hasClient())
					{
						pumpSocket(10);
					}
					else
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
				}
			});
	}

	void TransportWebSocketServer::stopBackgroundThread() noexcept
	{
		m_running.store(false, std::memory_order_release);
		if (m_thread.joinable())
		{
			try
			{
				m_thread.join();
			}
			catch (...)
			{
			}
		}
	}
} // namespace rg2
