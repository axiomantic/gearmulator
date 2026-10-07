/* t0_wine_proxy.cpp -- The Wine editor proxy and TransportSocketServer.
 * Tier T0: no artifact, no firmware, no external file.
 *
 * The property this file exists to hold: the user-mode proxy DLL allows
 * the Clavia Windows editor to discover the virtual device, register its
 * completion event, handle the 3 private IOCTLs, and stream bulk data
 * bidirectionally over a loopback TCP socket to rg2Lib's TransportHub.
 *
 * Nothing in this file is an assert() and nothing catches an exception.
 * Every run-time verdict reports through failures; every compile-time
 * verdict is a static_assert.
 */

#include "../../rg2WineProxy/rg2usb.h"
#include "../../rg2WineProxy/setupapi_proxy.h"
#include "../transportHub.h"
#include "../transportWebSocket.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
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

	bool sameBytes(const uint8_t* const observed, const size_t observedSize, const uint8_t* const expected,
				   const size_t expectedSize)
	{
		if (observed == nullptr || observedSize != expectedSize)
			return false;
		for (size_t i = 0; i < expectedSize; ++i)
		{
			if (observed[i] != expected[i])
				return false;
		}
		return true;
	}

	void fillPattern(uint8_t* const dst, const size_t size, const uint32_t seed)
	{
		for (size_t i = 0; i < size; ++i)
			dst[i] = static_cast<uint8_t>(seed * 31u + i * 7u + 1u);
	}
} // namespace

int main()
{
	/* ------------------------------------------------------------- case 1.
	 * Compile-time contracts. TransportWebSocketServer must derive from
	 * TransportEndpoint directly. */
	static_assert(std::is_base_of_v<rg2::TransportEndpoint, rg2::TransportWebSocketServer>,
				  "TransportWebSocketServer must derive from TransportEndpoint");

	static_assert(rg2::wine::kIoctlRegisterEvent == 0x222000u, "IOCTL 0x222000 registers completion event");
	static_assert(rg2::wine::kIoctlUnregisterEvent == 0x222004u, "IOCTL 0x222004 unregisters completion event");
	static_assert(rg2::wine::kIoctlQueryEvent == 0x222008u, "IOCTL 0x222008 queries event registration status");
	static_assert(rg2::wine::kIoctlGetNotification == 0x22200Cu, "IOCTL 0x22200C dequeues 16-byte notification record");

	/* ------------------------------------------------------------ case 1b.
	 * Clavia interface GUID and virtual device path identification. */
	check(rg2::wine::isClaviaDeviceGuid("{CB3ED981-6125-4047-BC2A-292E370CC89A}"), "GUID string matches in uppercase");
	check(rg2::wine::isClaviaDeviceGuid("{cb3ed981-6125-4047-bc2a-292e370cc89a}"),
		  "GUID string matches case-insensitively");
	check(!rg2::wine::isClaviaDeviceGuid("{11111111-2222-3333-4444-555555555555}"),
		  "Non-matching GUID string rejected");

	const uint8_t validGuidBytes[16] = {0x81, 0xD9, 0x3E, 0xCB, 0x25, 0x61, 0x47, 0x40,
										0xBC, 0x2A, 0x29, 0x2E, 0x37, 0x0C, 0xC8, 0x9A};
	check(rg2::wine::isClaviaDeviceGuid(validGuidBytes), "Binary GUID matches");

	const uint8_t invalidGuidBytes[16] = {0};
	check(!rg2::wine::isClaviaDeviceGuid(invalidGuidBytes), "Invalid binary GUID rejected");

	const std::string devPath = rg2::wine::getVirtualDevicePath();
	check(devPath.find("cb3ed981-6125-4047-bc2a-292e370cc89a") != std::string::npos,
		  "Virtual device path carries Clavia GUID");

	/* ------------------------------------------------------------- case 2.
	 * Server listening and client connection over loopback. */
	rg2::TransportHub hub(1024, 16);
	rg2::TransportWebSocketServer server(hub, 0); // 0 selects ephemeral port

	check(server.listen(), "server binds and listens on loopback");
	const uint16_t port = server.port();
	check(port > 0, "server reports bound port > 0");

	rg2::wine::G2UsbDeviceSession session;
	check(!session.isConnected(), "session initially disconnected");

	std::thread serverThread(
		[&server]()
		{
			if (server.acceptClient(true))
			{
				server.pumpSocket(100);
			}
		});

	check(session.connectToHub("127.0.0.1", port), "session connects to server");
	serverThread.join();

	check(server.hasClient(), "server confirms client connected");
	check(server.isUpgraded(), "server confirms client upgraded to websocket");
	check(session.isConnected(), "session confirms connected");

	/* ------------------------------------------------------------- case 3.
	 * Event registration lifecycle (IOCTLs 0x222000, 0x222008, 0x222004). */
	uint64_t signaledCount = 0;
	session.setEventSignaler([&signaledCount](uintptr_t) { ++signaledCount; });

	bool registered = false;
	check(session.queryEvent(registered), "queryEvent succeeds");
	check(!registered, "no event initially registered");

	const uintptr_t dummyEventHandle = 0x12345678;
	check(session.registerEvent(dummyEventHandle), "registerEvent succeeds");
	check(session.queryEvent(registered), "queryEvent succeeds after registration");
	check(registered, "event confirmed registered");

	check(session.unregisterEvent(), "unregisterEvent succeeds");
	check(session.queryEvent(registered), "queryEvent succeeds after unregister");
	check(!registered, "event confirmed unregistered");

	// Re-register event for remaining cases
	check(session.registerEvent(dummyEventHandle), "re-register event succeeds");

	/* ------------------------------------------------------------- case 4.
	 * Bulk OUT: session::writeFile -> socket -> server -> hub -> drain. */
	uint8_t outPayload[48];
	fillPattern(outPayload, sizeof(outPayload), 42u);

	size_t written = 0;
	check(session.writeFile(outPayload, sizeof(outPayload), written), "writeFile succeeds");
	checkEqual(written, sizeof(outPayload), "writeFile sent all bytes");

	// Server pumps socket to read frame and hand to hub
	size_t pumped = server.pumpSocket();
	checkEqual(pumped, 1u, "server received 1 frame from client");

	rg2::StampedFrame drained[4];
	const size_t drainedCount = hub.drainToDevice(drained, 4);
	checkEqual(drainedCount, 1u, "hub drained 1 frame to device");
	checkEqual(drained[0].frame.size, sizeof(outPayload), "drained frame size matches outbound payload");
	check(sameBytes(drained[0].frame.data, drained[0].frame.size, outPayload, sizeof(outPayload)),
		  "drained frame bytes match outbound payload");

	/* ------------------------------------------------------------- case 5.
	 * Notification egress: device -> hub -> server -> socket -> session dequeue.
	 * 16-byte record with type 2 (Embedded notification). */
	uint8_t notifRecord[16];
	fillPattern(notifRecord, sizeof(notifRecord), 7u);
	notifRecord[0] = 0x22; // Type 2 (Embedded), length 2

	const uint64_t signalsBefore = signaledCount;
	hub.fromDevice(rg2::ProtocolFrame{notifRecord, sizeof(notifRecord)});

	// Session pumps socket to receive incoming frames
	const size_t clientPumped = session.pumpSocket();
	check(clientPumped > 0, "session pumped notification frame");
	check(signaledCount > signalsBefore, "SetEvent was signaled on notification arrival");

	uint8_t dequeued[16];
	const size_t popped = session.dequeueNotification(dequeued);
	checkEqual(popped, 16u, "dequeueNotification returned 16 bytes");
	check(sameBytes(dequeued, sizeof(dequeued), notifRecord, sizeof(notifRecord)),
		  "dequeued notification bytes match emitted record");

	// Ring is now empty; second dequeue should return 0
	checkEqual(session.dequeueNotification(dequeued), 0u, "dequeueNotification on empty ring returns 0");

	/* ------------------------------------------------------------- case 6.
	 * Bulk IN egress: device -> hub -> server -> socket -> session readFile. */
	uint8_t bulkInPayload[64];
	fillPattern(bulkInPayload, sizeof(bulkInPayload), 99u);

	hub.fromDevice(rg2::ProtocolFrame{bulkInPayload, sizeof(bulkInPayload)});

	session.pumpSocket();
	uint8_t readBuffer[128];
	size_t bytesRead = 0;
	check(session.readFile(readBuffer, sizeof(readBuffer), bytesRead), "readFile succeeds");
	checkEqual(bytesRead, sizeof(bulkInPayload), "readFile read all bytes");
	check(sameBytes(readBuffer, bytesRead, bulkInPayload, sizeof(bulkInPayload)),
		  "read bytes match device emitted bulk payload");

	/* ------------------------------------------------------------- case 7.
	 * Clean disconnect. */
	session.disconnect();
	check(!session.isConnected(), "session disconnected cleanly");
	server.close();
	check(!server.hasClient(), "server client released");

	if (failures == 0)
		printf("PASS t0_wine_proxy: all cases passed\n");
	return failures == 0 ? 0 : 1;
}
