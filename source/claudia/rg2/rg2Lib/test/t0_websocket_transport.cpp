/* t0_websocket_transport.cpp -- The RFC 6455 WebSocket transport server.
 * Tier T0: no artifact, no firmware, no external file.
 *
 * Asserts compile-time contracts, RFC 6455 test vector compliance, loopback
 * handshake negotiation, binary frame unmasking/dispatching to TransportHub,
 * device-to-client frame wrapping, and clean disconnects.
 */

#include "../transportHub.h"
#include "../transportWebSocket.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if (!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqual(const uint64_t observed, const uint64_t expected, const char* const what)
	{
		if (observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what, static_cast<unsigned long long>(observed),
				   static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	class MockEndpoint final : public rg2::TransportEndpoint
	{
	public:
		void onFrameFromDevice(rg2::ProtocolFrame _frame) noexcept override
		{
			if (_frame.data && _frame.size > 0)
			{
				received.assign(_frame.data, _frame.data + _frame.size);
				++callCount;
			}
		}

		std::vector<uint8_t> received;
		uint64_t callCount = 0;
	};

	int connectClientSocket(const uint16_t port)
	{
		const int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s < 0)
			return -1;

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
		{
#ifdef _WIN32
			::closesocket(s);
#else
			::close(s);
#endif
			return -1;
		}
		return s;
	}

	void closeSocket(int fd)
	{
		if (fd >= 0)
		{
#ifdef _WIN32
			::closesocket(fd);
#else
			::close(fd);
#endif
		}
	}
} // namespace

int main()
{
	/* ------------------------------------------------------------- case 1.
	 * Compile-time contracts. TransportWebSocketServer must derive from
	 * TransportEndpoint directly. */
	static_assert(std::is_base_of_v<rg2::TransportEndpoint, rg2::TransportWebSocketServer>,
				  "TransportWebSocketServer must derive from TransportEndpoint");

	/* ------------------------------------------------------------- case 2.
	 * RFC 6455 Section 1.3 standard test vector. */
	const std::string rfcTestKey = "dGhlIHNhbXBsZSBub25jZQ==";
	const std::string expectedAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
	const std::string observedAccept = rg2::TransportWebSocketServer::computeAcceptKey(rfcTestKey);
	check(observedAccept == expectedAccept, "RFC 6455 computeAcceptKey matches official test vector");

	/* ------------------------------------------------------------- case 3.
	 * Server listening and ephemeral port selection. */
	rg2::TransportHub hub(1024, 16);
	rg2::TransportWebSocketServer server(hub, 0); // ephemeral port

	check(server.listen(), "server binds and listens on loopback");
	const uint16_t port = server.port();
	check(port > 0, "server reports bound port > 0");

	/* ------------------------------------------------------------- case 4.
	 * Client connects and performs HTTP Upgrade handshake. */
	const int clientFd = connectClientSocket(port);
	check(clientFd >= 0, "client connects to server");

	check(server.acceptClient(true), "server accepts client connection");
	check(server.hasClient(), "server confirms client connected");
	check(!server.isUpgraded(), "server is not upgraded before handshake");

	// Send HTTP Upgrade request
	const std::string httpRequest = "GET / HTTP/1.1\r\n"
									"Host: 127.0.0.1\r\n"
									"Upgrade: websocket\r\n"
									"Connection: Upgrade\r\n"
									"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
									"Sec-WebSocket-Version: 13\r\n"
									"\r\n";
	::send(clientFd, httpRequest.data(), httpRequest.size(), 0);

	// Pump server to process handshake
	server.pumpSocket(100);
	check(server.isUpgraded(), "server upgraded to WebSocket after handshake");

	// Read HTTP 101 response on client
	char respBuf[1024] = {0};
	const long n = ::recv(clientFd, respBuf, sizeof(respBuf) - 1, 0);
	check(n > 0, "client received handshake response");
	std::string response(respBuf, n > 0 ? n : 0);
	check(response.find("101 Switching Protocols") != std::string::npos, "response status 101");
	check(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
		  "response carries correct Sec-WebSocket-Accept header");

	/* ------------------------------------------------------------- case 5.
	 * Client to Server: Masked binary frame forwarding into TransportHub. */
	// Channel 0 (Bulk) + 4-byte payload [0xAA, 0xBB, 0xCC, 0xDD] -> 5 bytes total payload
	const uint8_t maskKey[4] = {0x12, 0x34, 0x56, 0x78};
	const uint8_t rawPayload[5] = {0x00, 0xAA, 0xBB, 0xCC, 0xDD};
	uint8_t maskedPayload[5];
	for (size_t i = 0; i < 5; ++i)
		maskedPayload[i] = rawPayload[i] ^ maskKey[i % 4];

	std::vector<uint8_t> wsFrame;
	wsFrame.push_back(0x82); // FIN | Binary opcode 2
	wsFrame.push_back(0x80 | 5); // Masked | length 5
	wsFrame.insert(wsFrame.end(), maskKey, maskKey + 4);
	wsFrame.insert(wsFrame.end(), maskedPayload, maskedPayload + 5);

	::send(clientFd, reinterpret_cast<const char*>(wsFrame.data()), wsFrame.size(), 0);

	const size_t delivered = server.pumpSocket(100);
	checkEqual(delivered, 1, "server delivered 1 frame to hub");
	checkEqual(server.framesIn(), 1, "framesIn incremented");

	// Verify hub received the payload (minus channel prefix) at quantum boundary
	rg2::StampedFrame stampedFrames[16];
	const size_t drained = hub.drainToDevice(stampedFrames, 16);
	checkEqual(drained, 1, "hub drained 1 stamped frame");
	checkEqual(stampedFrames[0].frame.size, 4, "payload size is 4 bytes");
	check(stampedFrames[0].frame.data[0] == 0xAA && stampedFrames[0].frame.data[1] == 0xBB &&
			  stampedFrames[0].frame.data[2] == 0xCC && stampedFrames[0].frame.data[3] == 0xDD,
		  "payload bytes match unmasked client data");

	/* ------------------------------------------------------------- case 6.
	 * Server to Client: onFrameFromDevice generates valid WebSocket binary frame. */
	const uint8_t synthReply[3] = {0x42, 0x43, 0x44};
	server.onFrameFromDevice(rg2::ProtocolFrame{synthReply, 3});
	checkEqual(server.framesOut(), 1, "framesOut incremented");

	// Read frame on client
	uint8_t clientRx[64] = {0};
	const long rxLen = ::recv(clientFd, reinterpret_cast<char*>(clientRx), sizeof(clientRx), 0);
	check(rxLen >= 6, "client received at least 6 bytes (2 header + 1 channel + 3 payload)");
	checkEqual(clientRx[0], 0x82, "client received binary frame opcode");
	checkEqual(clientRx[1], 4, "client received payload length (1 channel + 3 data = 4)");
	checkEqual(clientRx[2], 0, "client received Bulk channel 0");
	check(clientRx[3] == 0x42 && clientRx[4] == 0x43 && clientRx[5] == 0x44, "client payload matches device frame");

	/* ------------------------------------------------------------- case 7.
	 * 16-byte frame correctly tagged with Notification channel 1. */
	uint8_t notifData[16];
	for (int i = 0; i < 16; ++i)
		notifData[i] = static_cast<uint8_t>(i);
	server.onFrameFromDevice(rg2::ProtocolFrame{notifData, 16});
	checkEqual(server.framesOut(), 2, "framesOut incremented for notification");

	const long notifRxLen = ::recv(clientFd, reinterpret_cast<char*>(clientRx), sizeof(clientRx), 0);
	check(notifRxLen >= 19, "client received 2 header + 1 channel + 16 payload = 19 bytes");
	checkEqual(clientRx[0], 0x82, "notification frame binary opcode");
	checkEqual(clientRx[1], 17, "notification frame length 17");
	checkEqual(clientRx[2], 1, "notification channel byte is 1");

	/* ------------------------------------------------------------- case 8.
	 * Ping / Pong handling. */
	const uint8_t pingFrame[6] = {0x89, 0x80, 0x01, 0x02, 0x03, 0x04}; // Masked 0-length ping
	::send(clientFd, reinterpret_cast<const char*>(pingFrame), sizeof(pingFrame), 0);
	server.pumpSocket(100);

	const long pongLen = ::recv(clientFd, reinterpret_cast<char*>(clientRx), sizeof(clientRx), 0);
	check(pongLen >= 2, "client received pong response");
	checkEqual(clientRx[0], 0x8A, "pong opcode is 0x8A");

	/* ------------------------------------------------------------- case 9.
	 * Clean close. */
	closeSocket(clientFd);
	server.pumpSocket(50);
	check(!server.hasClient(), "server detects client disconnect");

	/* ------------------------------------------------------------ case 10.
	 * Multi-instance auto-port probing. */
	rg2::TransportHub hub2(1024, 16);
	rg2::TransportWebSocketServer server2(hub2, port); // requests same port as server 1!
	check(server2.listen(), "second server binds when first occupies requested port");
	check(server2.port() > port, "second server auto-probed and bound to higher port");
	server2.close();

	/* ------------------------------------------------------------ case 11.
	 * Non-WebSocket HTTP requests rejected with 400 Bad Request. */
	const int httpFd = connectClientSocket(port);
	check(httpFd >= 0, "HTTP client connects to server");
	check(server.acceptClient(true), "server accepts HTTP client");

	const std::string getRequest = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
	::send(httpFd, getRequest.data(), getRequest.size(), 0);
	server.pumpSocket(100);

	char httpResp[1024] = {0};
	const long httpLen = ::recv(httpFd, httpResp, sizeof(httpResp) - 1, 0);
	check(httpLen > 0, "HTTP response received");
	const std::string httpRespStr(httpResp, httpLen > 0 ? httpLen : 0);
	check(httpRespStr.find("HTTP/1.1 400 Bad Request") != std::string::npos, "HTTP 400 Bad Request returned");
	check(httpRespStr.find("Connection: close") != std::string::npos, "Connection: close header present");
	closeSocket(httpFd);

	/* ------------------------------------------------------------ case 12.
	 * Origin validation: localhost/trusted origins allowed, untrusted rejected with 403 Forbidden. */
	// 12a. Untrusted origin
	const int untrustedFd = connectClientSocket(port);
	check(untrustedFd >= 0, "untrusted client connects");
	check(server.acceptClient(true), "server accepts untrusted client");

	const std::string untrustedRequest = "GET / HTTP/1.1\r\n"
										 "Host: 127.0.0.1\r\n"
										 "Upgrade: websocket\r\n"
										 "Connection: Upgrade\r\n"
										 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
										 "Sec-WebSocket-Version: 13\r\n"
										 "Origin: https://malicious-evil-site.com\r\n"
										 "\r\n";
	::send(untrustedFd, untrustedRequest.data(), untrustedRequest.size(), 0);
	server.pumpSocket(100);

	char untrustedResp[1024] = {0};
	const long untrustedLen = ::recv(untrustedFd, untrustedResp, sizeof(untrustedResp) - 1, 0);
	check(untrustedLen > 0, "untrusted origin received response");
	const std::string untrustedRespStr(untrustedResp, untrustedLen > 0 ? untrustedLen : 0);
	check(untrustedRespStr.find("HTTP/1.1 403 Forbidden") != std::string::npos,
		  "untrusted origin rejected with 403 Forbidden");
	check(!server.isUpgraded(), "server is not upgraded for forbidden origin");
	closeSocket(untrustedFd);

	// 12b. Trusted localhost origin
	const int localOriginFd = connectClientSocket(port);
	check(localOriginFd >= 0, "local origin client connects");
	check(server.acceptClient(true), "server accepts local origin client");

	const std::string localOriginRequest = "GET / HTTP/1.1\r\n"
										   "Host: 127.0.0.1\r\n"
										   "Upgrade: websocket\r\n"
										   "Connection: Upgrade\r\n"
										   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
										   "Sec-WebSocket-Version: 13\r\n"
										   "Origin: http://localhost:7777\r\n"
										   "\r\n";
	::send(localOriginFd, localOriginRequest.data(), localOriginRequest.size(), 0);
	server.pumpSocket(100);
	check(server.isUpgraded(), "server upgraded for trusted localhost origin");

	char localResp[1024] = {0};
	const long localLen = ::recv(localOriginFd, localResp, sizeof(localResp) - 1, 0);
	check(localLen > 0, "local origin received response");
	const std::string localRespStr(localResp, localLen > 0 ? localLen : 0);
	check(localRespStr.find("101 Switching Protocols") != std::string::npos,
		  "local origin received 101 Switching Protocols");
	check(localRespStr.find("Access-Control-Allow-Private-Network: true") != std::string::npos,
		  "response contains Access-Control-Allow-Private-Network");
	closeSocket(localOriginFd);
	server.disconnectClient();

	/* ------------------------------------------------------------ case 13.
	 * Optional token validation: rejected when missing/incorrect, accepted when matching. */
	server.setAuthToken("nord_secret_token_42");
	check(server.authToken() == "nord_secret_token_42", "auth token is configured");

	// 13a. Missing token rejected with 401 Unauthorized
	const int noTokenFd = connectClientSocket(port);
	check(noTokenFd >= 0, "no-token client connects");
	check(server.acceptClient(true), "server accepts no-token client");

	const std::string noTokenRequest = "GET / HTTP/1.1\r\n"
									   "Host: 127.0.0.1\r\n"
									   "Upgrade: websocket\r\n"
									   "Connection: Upgrade\r\n"
									   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
									   "Sec-WebSocket-Version: 13\r\n"
									   "\r\n";
	::send(noTokenFd, noTokenRequest.data(), noTokenRequest.size(), 0);
	server.pumpSocket(100);

	char noTokenResp[1024] = {0};
	const long noTokenLen = ::recv(noTokenFd, noTokenResp, sizeof(noTokenResp) - 1, 0);
	check(noTokenLen > 0, "no-token client received response");
	const std::string noTokenRespStr(noTokenResp, noTokenLen > 0 ? noTokenLen : 0);
	check(noTokenRespStr.find("HTTP/1.1 401 Unauthorized") != std::string::npos,
		  "missing token rejected with 401 Unauthorized");
	check(!server.isUpgraded(), "server is not upgraded without required token");
	closeSocket(noTokenFd);

	// 13b. Valid token in query param (?token=nord_secret_token_42) succeeds
	const int tokenFd = connectClientSocket(port);
	check(tokenFd >= 0, "token client connects");
	check(server.acceptClient(true), "server accepts token client");

	const std::string tokenRequest = "GET /?token=nord_secret_token_42 HTTP/1.1\r\n"
									 "Host: 127.0.0.1\r\n"
									 "Upgrade: websocket\r\n"
									 "Connection: Upgrade\r\n"
									 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
									 "Sec-WebSocket-Version: 13\r\n"
									 "\r\n";
	::send(tokenFd, tokenRequest.data(), tokenRequest.size(), 0);
	server.pumpSocket(100);
	check(server.isUpgraded(), "server upgraded for client with matching token");
	closeSocket(tokenFd);
	server.disconnectClient();

	server.setAuthToken(""); // Reset token to unauthenticated default

	/* ------------------------------------------------------------ case 14.
	 * De-framing memory efficiency: multiple frames in a single socket read
	 * exercise m_rxOffset cursor advancing and compaction. */
	const int multiFd = connectClientSocket(port);
	check(multiFd >= 0, "multi-frame client connects");
	check(server.acceptClient(true), "server accepts multi-frame client");

	const std::string multiHandshake = "GET / HTTP/1.1\r\n"
									   "Host: 127.0.0.1\r\n"
									   "Upgrade: websocket\r\n"
									   "Connection: Upgrade\r\n"
									   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
									   "Sec-WebSocket-Version: 13\r\n"
									   "\r\n";
	::send(multiFd, multiHandshake.data(), multiHandshake.size(), 0);
	server.pumpSocket(100);
	check(server.isUpgraded(), "server upgraded for multi-frame test");

	char drainBuf[1024];
	::recv(multiFd, drainBuf, sizeof(drainBuf), 0);

	std::vector<uint8_t> burst;
	const uint8_t mKey[4] = {0x11, 0x22, 0x33, 0x44};
	const uint8_t pA[3] = {0x00, 0xA1, 0xA2};
	burst.push_back(0x82);
	burst.push_back(0x80 | 3);
	burst.insert(burst.end(), mKey, mKey + 4);
	for (size_t i = 0; i < 3; ++i)
		burst.push_back(pA[i] ^ mKey[i % 4]);

	const uint8_t pB[3] = {0x00, 0xB1, 0xB2};
	burst.push_back(0x82);
	burst.push_back(0x80 | 3);
	burst.insert(burst.end(), mKey, mKey + 4);
	for (size_t i = 0; i < 3; ++i)
		burst.push_back(pB[i] ^ mKey[i % 4]);

	::send(multiFd, reinterpret_cast<const char*>(burst.data()), burst.size(), 0);
	const size_t burstDelivered = server.pumpSocket(100);
	checkEqual(burstDelivered, 2, "both frames delivered in single pumpSocket call");

	rg2::StampedFrame burstFrames[4];
	const size_t drainedBurst = hub.drainToDevice(burstFrames, 4);
	checkEqual(drainedBurst, 2, "hub received 2 burst frames");
	check(burstFrames[0].frame.size == 2 && burstFrames[0].frame.data[0] == 0xA1 &&
			  burstFrames[0].frame.data[1] == 0xA2,
		  "first burst frame payload matches");
	check(burstFrames[1].frame.size == 2 && burstFrames[1].frame.data[0] == 0xB1 &&
			  burstFrames[1].frame.data[1] == 0xB2,
		  "second burst frame payload matches");

	closeSocket(multiFd);
	server.disconnectClient();

	server.close();

	if (failures == 0)
		printf("ALL TESTS PASSED\n");
	return failures;
}
