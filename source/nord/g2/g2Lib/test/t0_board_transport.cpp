/* Tier T0: no artifact, no firmware, no file outside this repository.
 *
 * The property this file exists to hold: a protocol frame a plugin originates
 * crosses the quantum boundary into the Board's USB device, and a frame the
 * device emits crosses back out to the attachments -- with the internal client
 * as a peer of the usbip endpoint and not a path through it.
 *
 * Every case is probed from outside the thing under test. None of them reads a
 * word the Board prints about itself.
 *
 * Case 3 calls tickSofIfDue twice for one release because the slots a drain
 * hands out stay borrowed by the caller until the start of the following
 * drain. The first quantum boundary drains and lends, and the second releases.
 * A test that pumped once and expected a free queue would be asserting against
 * a lifetime the hub deliberately does not have.
 *
 * Nothing in this file is an assert() and nothing catches an exception. Every
 * run-time verdict reports through this file's own failure counter, so NDEBUG
 * changes no case; every compile-time verdict is a static_assert.
 */
#include "../board.h"
#include "../internalClient.h"
#include "../transportHub.h"
#include "../../g2JucePlugin/g2PatchLoad.h"
#include "../crc16.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <vector>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqual(const uint64_t observed, const uint64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what,
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	/* Byte content, compared whole. A frame that arrived truncated, one that
	 * arrived with a stale pointer and one that arrived out of order are three
	 * different failures rather than one length mismatch. */
	bool sameBytes(const uint8_t* const observed, const size_t observedSize,
		const uint8_t* const expected, const size_t expectedSize)
	{
		if(observed == nullptr || observedSize != expectedSize)
			return false;
		for(size_t i = 0; i < expectedSize; ++i)
		{
			if(observed[i] != expected[i])
				return false;
		}
		return true;
	}

	void fillPattern(uint8_t* const dst, const size_t size, const uint32_t seed)
	{
		for(size_t i = 0; i < size; ++i)
			dst[i] = static_cast<uint8_t>(seed * 31u + i * 7u + 1u);
	}

	/* --------------------------------------------------------------- alloc.
	 * Every object driven inside an armed window is already constructed. */
	struct AllocStats
	{
		uint64_t calls = 0;
		uint64_t bytes = 0;
	};

	AllocStats g_alloc;
	bool       g_allocArmed = false;

	void armAlloc()    { g_alloc = AllocStats{}; g_allocArmed = true; }
	void disarmAlloc() { g_allocArmed = false; }

	/* A container whose bytes are a valid `.pch2` of this project's own
	 * specification: an ASCII header ended by NUL, a 2-byte binary header,
	 * objects of [type][2-byte big-endian length][payload], and a 2-byte
	 * stored CRC-16/XMODEM over the binary header and every object. It is
	 * built here and read from no file, which is what keeps this case T0. */
	std::vector<uint8_t> buildContainer(const std::vector<uint8_t>& _objects)
	{
		std::vector<uint8_t> file;

		const char* const ascii = "Version=Nord Modular G2 File Format 1\n";
		for(const char* p = ascii; *p != 0; ++p)
			file.push_back(static_cast<uint8_t>(*p));
		file.push_back(0);

		const size_t binaryHeader = file.size();

		file.push_back(0x17);   // version byte
		file.push_back(0x00);   // type byte

		for(const uint8_t b : _objects)
			file.push_back(b);

		file.push_back(0);
		file.push_back(0);

		const uint16_t crc = g2::crc16File(file.data(), file.size(), binaryHeader);
		g2::crc16Store(file.data() + file.size() - 2, crc);

		return file;
	}

	void appendObject(std::vector<uint8_t>& _dst, const uint8_t _type,
		const size_t _length, const uint32_t _seed)
	{
		_dst.push_back(_type);
		_dst.push_back(static_cast<uint8_t>((_length >> 8) & 0xffu));
		_dst.push_back(static_cast<uint8_t>(_length & 0xffu));

		const size_t at = _dst.size();
		_dst.resize(at + _length);
		fillPattern(_dst.data() + at, _length, _seed);
	}
}

void* operator new(const size_t size)
{
	if(g_allocArmed)
	{
		++g_alloc.calls;
		g_alloc.bytes += size;
	}
	void* const p = std::malloc(size == 0 ? 1 : size);
	if(p == nullptr)
		throw std::bad_alloc();
	return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

int main()
{
	/* ------------------------------------------------------------- case 1.
	 * The compile-time half. The Board owns a hub -- `transport()` answers a
	 * g2::TransportHub& and not a pointer, so there is no state in which the
	 * Board has none -- and the hub still reserves exactly three
	 * attachments. */
	static_assert(std::is_same_v<decltype(std::declval<g2::Board&>().transport()),
	                             g2::TransportHub&>,
	              "Board::transport must answer the Board's own hub by reference");
	static_assert(g2::TransportHub::kMaxEndpoints == 3,
	              "design section 15.1 fixes three attachments");

	g2::Board board;

	/* ------------------------------------------------------------- case 2.
	 * Egress. `Board::onUsbTx` is the exact function pointer the Board hands
	 * to isp1181_create, and this case drives that pointer rather than a
	 * private forwarding helper.
	 *
	 * The verdict is read at the client and not at the Board. */
	{
		g2::InternalClient client(board.transport(), 512, 4);

		uint8_t outbound[37];
		fillPattern(outbound, sizeof(outbound), 9u);

		g2::Board::onUsbTx(&board, 2, outbound, sizeof(outbound));

		g2::ProtocolFrame got{};
		check(client.receive(got),
			"a frame the device emitted reaches the only attachment");
		check(sameBytes(got.data, got.size, outbound, sizeof(outbound)),
			"the frame the attachment receives is the device's bytes, whole");
		checkEqual(client.droppedFrames(), 0u,
			"the device's frame was not dropped");

		/* The client is the only attachment in this window. Nothing else is
		 * attached to route through, so a client that reached the device
		 * through another endpoint could not have received a byte here. */
	}

	/* ------------------------------------------------------------- case 3.
	 * Ingress, observed through the hub's back-pressure. The queue is filled
	 * to its depth, which is what makes the next send refuse; then the Board is
	 * driven across quantum boundaries and the send is offered again. Only a
	 * drain that really ran can relieve that refusal, and the relief is read
	 * from the hub and not from anything the Board says.
	 *
	 * It does not claim the bytes reached the ISP1181's endpoint buffer: this
	 * case reads the hub, and the hub is relieved by a drain whatever the
	 * device then does with the frame. */
	{
		g2::InternalClient client(board.transport(), 512, 4);

		uint8_t payload[16];
		fillPattern(payload, sizeof(payload), 3u);

		size_t accepted = 0;
		while(accepted < 4096 &&
		      client.send(g2::ProtocolFrame{ payload, sizeof(payload) }))
		{
			++accepted;
		}

		check(accepted > 0,
			"the hub accepts at least one frame before it fills");
		check(accepted < 4096,
			"the hub refuses once its queue is full, rather than accepting forever");

		check(!client.send(g2::ProtocolFrame{ payload, sizeof(payload) }),
			"a full queue refuses, which is the precondition this case measures against");

		/* The first boundary drains and lends; the second releases. */
		board.tickSofIfDue(0);
		check(!client.send(g2::ProtocolFrame{ payload, sizeof(payload) }),
			"one drain lends the slots out and does NOT free them");

		board.tickSofIfDue(1);
		check(client.send(g2::ProtocolFrame{ payload, sizeof(payload) }),
			"the Board drains its hub at the quantum boundary, which frees the queue");
	}

	/* ------------------------------------------------------------- case 4.
	 * What the Board hands the device is what the plugin originated, verbatim
	 * and in order. A second Board is used and it is never pumped, so this file
	 * performs the drain itself and reads the bytes -- an independent probe of
	 * the same hub the Board's own pump reads. */
	{
		g2::Board second;
		g2::InternalClient client(second.transport(), 512, 8);

		uint8_t a[11], b[23], c[5];
		fillPattern(a, sizeof(a), 41u);
		fillPattern(b, sizeof(b), 42u);
		fillPattern(c, sizeof(c), 43u);

		check(client.send(g2::ProtocolFrame{ a, sizeof(a) }), "send a");
		check(client.send(g2::ProtocolFrame{ b, sizeof(b) }), "send b");
		check(client.send(g2::ProtocolFrame{ c, sizeof(c) }), "send c");

		g2::StampedFrame drained[8]{};
		const size_t n = second.transport().drainToDevice(drained, 8);

		checkEqual(n, 3u, "the drain yields exactly the three originated frames");

		if(n == 3)
		{
			check(sameBytes(drained[0].frame.data, drained[0].frame.size, a, sizeof(a)),
				"drained frame 0 is the first originated frame, whole");
			check(sameBytes(drained[1].frame.data, drained[1].frame.size, b, sizeof(b)),
				"drained frame 1 is the second originated frame, whole");
			check(sameBytes(drained[2].frame.data, drained[2].frame.size, c, sizeof(c)),
				"drained frame 2 is the third originated frame, whole");
			checkEqual(drained[0].frameIndex, drained[2].frameIndex,
				"three frames that crossed in one quantum carry one frame index");
		}
	}

	/* ------------------------------------------------------------- case 5.
	 * A whole container reaches the drain. The file is built in this process,
	 * loaded through the same pch2Load a plugin calls, and every object is
	 * compared byte for byte against the file at the offset it came from --
	 * header included, because the frame is the object verbatim. */
	{
		std::vector<uint8_t> objects;
		appendObject(objects, 0x21, 15, 71u);
		appendObject(objects, 0x4A, 64, 72u);
		appendObject(objects, 0x5A, 40, 73u);
		appendObject(objects, 0x5B, 84, 74u);
		appendObject(objects, 0x6F, 0,  75u);

		const std::vector<uint8_t> file = buildContainer(objects);

		g2::Board third;
		g2::InternalClient client(third.transport(), 4096, 16);

		const g2::Pch2LoadResult result = g2::pch2Load(file.data(), file.size(), client);

		checkEqual(static_cast<uint64_t>(result),
			static_cast<uint64_t>(g2::Pch2LoadResult::Loaded),
			"a container carrying every type this project's specification names loads");

		g2::StampedFrame drained[16]{};
		const size_t n = third.transport().drainToDevice(drained, 16);

		checkEqual(n, 5u, "one frame crossed for each object in the container");

		if(n == 5)
		{
			size_t at = 0;
			for(size_t i = 0; i < 5; ++i)
			{
				const size_t length =
					(static_cast<size_t>(objects[at + 1]) << 8) | objects[at + 2];

				check(sameBytes(drained[i].frame.data, drained[i].frame.size,
						objects.data() + at, 3 + length),
					"the frame carries the object verbatim, its 3-byte header included");

				at += 3 + length;
			}
			checkEqual(at, objects.size(), "every object of the container was carried");
		}
	}

	/* ------------------------------------------------------------- case 6.
	 * The pump allocates nothing. The drain target is the Board's own storage,
	 * sized once with the hub. */
	{
		g2::InternalClient client(board.transport(), 512, 4);

		uint8_t payload[8];
		fillPattern(payload, sizeof(payload), 5u);

		client.send(g2::ProtocolFrame{ payload, sizeof(payload) });

		armAlloc();
		for(uint64_t frame = 0; frame < 8; ++frame)
			board.tickSofIfDue(frame);
		const AllocStats window = g_alloc;
		disarmAlloc();

		checkEqual(window.calls, 0u,
			"no allocation call was made while the Board pumped its transport");
		checkEqual(window.bytes, 0u,
			"no byte was allocated while the Board pumped its transport");
	}

	/* ------------------------------------------------------------- case 7.
	 * The drain runs exactly once for each quantum boundary, counted. The
	 * hub's frame index is its own quantum ordinal and the first drain stamps
	 * 0, so the stamp on a frame drained after N boundaries have passed is the
	 * number of drains those boundaries performed.
	 *
	 * Case 3 only asks whether the queue was freed, which two drains free just
	 * as well as one; a pump that drained twice would make the hub's ordinal
	 * run at twice the machine's rate, and every frame that crossed together
	 * would be stamped apart. */
	{
		g2::Board counted;
		g2::InternalClient client(counted.transport(), 512, 4);

		constexpr uint64_t boundaries = 7;

		for(uint64_t f = 0; f < boundaries; ++f)
			counted.tickSofIfDue(f);

		uint8_t payload[4];
		fillPattern(payload, sizeof(payload), 6u);

		check(client.send(g2::ProtocolFrame{ payload, sizeof(payload) }),
			"a frame is originated after the counted boundaries have passed");

		/* This drain is the (boundaries + 1)-th, so it stamps `boundaries`. */
		g2::StampedFrame drained[4]{};
		const size_t n = counted.transport().drainToDevice(drained, 4);

		checkEqual(n, 1u, "the counted board's hub holds exactly the one frame");

		if(n == 1)
		{
			checkEqual(drained[0].frameIndex, boundaries,
				"the Board drained its hub exactly once for each quantum boundary");
			check(sameBytes(drained[0].frame.data, drained[0].frame.size,
					payload, sizeof(payload)),
				"the counted board's frame is the originated bytes, whole");
		}
	}

	/* ------------------------------------------------------------- case 8.
	 * The split, and the planted control that brings the defect back.
	 *
	 * `Fifo.accept` in the device model refuses for exactly two reasons -- the
	 * buffer is full, or the packet is larger than the buffer -- and a refusal
	 * on its own does not say which. This case removes the first reason by
	 * construction: it is the first offer ever made to a freshly constructed
	 * Board, so that endpoint's buffer cannot hold anything, and a refusal here
	 * can only be the size one.
	 *
	 * The two arms differ in one field. Both push the same 169-byte frame --
	 * the length of the first object the real `.pch2` corpus refuses, taken
	 * from that measurement rather than chosen. The control arm sets
	 * `usbMaxPacketBytes` above the frame: one packet, whole frame, refused for
	 * size and held for ever.
	 *
	 * Nothing here boots a machine, so nothing drains the endpoint and neither
	 * arm can complete its frame. That is deliberate: what is measured here is
	 * the first offer, which is the one the two arms disagree about. */
	{
		constexpr size_t frameBytes = 169;

		uint8_t big[frameBytes];
		fillPattern(big, sizeof(big), 7u);

		/* The split arm. The first packet is a full max-packet-size piece of
		 * the frame and the empty buffer takes it. */
		{
			g2::BoardConfig cfg;
			check(cfg.usbMaxPacketBytes < frameBytes,
				"the split arm's packet size really is smaller than the frame,"
				" so this arm exercises a split at all");

			g2::Board split(cfg);
			g2::InternalClient client(split.transport(), 512, 4);

			check(client.send(g2::ProtocolFrame{ big, sizeof(big) }),
				"the oversized frame is originated on the split arm");

			split.tickSofIfDue(0);

			const g2::Board::UsbTransportStats u = split.usbTransport();

			checkEqual(u.drained,   1u, "the split arm drained the frame");
			checkEqual(u.offered,   1u, "the split arm offered exactly one packet");
			checkEqual(u.accepted,  1u,
				"THE SPLIT WORKS: an empty endpoint buffer TAKES the first"
				" max-packet-size piece of a frame it could never take whole");
			checkEqual(u.refused,   0u, "the split arm's first packet was not refused");
			checkEqual(u.completed, 0u,
				"one packet of several is not a completed frame");
			check(u.held, "the rest of the frame is still held");
			checkEqual(u.heldOffset, cfg.usbMaxPacketBytes,
				"the cursor advanced by exactly one packet");
			checkEqual(u.heldSize, frameBytes,
				"the held frame is the whole originated frame");
		}

		/* The planted control. The split is disabled by raising the packet
		 * size above the frame, and the size refusal returns. */
		{
			g2::BoardConfig cfg;
			cfg.usbMaxPacketBytes = frameBytes + 1;

			g2::Board whole(cfg);
			g2::InternalClient client(whole.transport(), 512, 4);

			check(client.send(g2::ProtocolFrame{ big, sizeof(big) }),
				"the oversized frame is originated on the control arm");

			whole.tickSofIfDue(0);

			const g2::Board::UsbTransportStats u = whole.usbTransport();

			checkEqual(u.drained,  1u, "the control arm drained the frame");
			checkEqual(u.offered,  1u, "the control arm offered exactly one packet");
			checkEqual(u.accepted, 0u,
				"PLANTED CONTROL: with the split disabled the device takes NOTHING");
			checkEqual(u.refused,  1u,
				"PLANTED CONTROL: the refusal returns, and on an untouched"
				" endpoint buffer it can only be the SIZE refusal");
			checkEqual(u.heldOffset, 0u,
				"a refused packet leaves the cursor where it was, so no byte"
				" crossed twice");

			/* It is not flow control. A full buffer is relieved by a drain;
			 * a packet larger than the buffer never is. Nothing drains here,
			 * but the distinction is still visible: the split arm above met
			 * the same buffer in the same state and was accepted, so the
			 * state is not what refused this one. */
			for(uint64_t f = 1; f < 16; ++f)
				whole.tickSofIfDue(f);

			const g2::Board::UsbTransportStats after = whole.usbTransport();

			checkEqual(after.accepted, 0u,
				"PLANTED CONTROL: fifteen further quanta do not help, because a"
				" packet longer than the buffer is not waiting for room");
			check(after.refused > u.refused,
				"the control arm keeps re-offering the frame rather than"
				" discarding it");
		}
	}

	/* ------------------------------------------------------------- case 9.
	 * The zero-length terminator, on a frame whose length is an exact multiple
	 * of the packet size.
	 *
	 * The convention that a bulk transfer of an exact multiple of the maximum
	 * packet size needs a trailing empty packet applies to no other length. A
	 * corpus that holds no object of such a length cannot exercise the path at
	 * all, so the frame is constructed here to exactly two packets.
	 *
	 * This case asserts what the Board does: off, the frame costs two packets;
	 * on, it costs three and the third carries no bytes. It does not assert
	 * which of the two the firmware wants. Neither ISP1362 Rev. 06 nor
	 * AN10008-01 states a bulk exact-multiple rule, so that remains unknown,
	 * and a case that asserted an answer would be inventing one. The answer is
	 * reachable by a flag rather than by a rewrite, and neither setting can
	 * drift in silence. */
	{
		g2::BoardConfig base;
		const size_t packet = base.usbMaxPacketBytes;

		check(packet != 0, "the packet size is non-zero, so an exact multiple exists");

		std::vector<uint8_t> exact(packet * 2);
		fillPattern(exact.data(), exact.size(), 8u);

		/* The terminator off -- the default. */
		{
			g2::Board off(base);
			g2::InternalClient client(off.transport(), 4096, 4);

			check(client.send(g2::ProtocolFrame{ exact.data(), exact.size() }),
				"the exact-multiple frame is originated with the terminator off");

			// One packet per quantum, and nothing drains this endpoint, so the
			// second packet needs the buffer freed. It is freed by taking it.
			off.tickSofIfDue(0);
			const g2::Board::UsbTransportStats first = off.usbTransport();

			checkEqual(first.accepted, 1u, "the first of two packets is taken");
			checkEqual(first.heldOffset, packet, "the cursor sits one packet in");
			checkEqual(first.completed, 0u, "one of two packets is not a frame");
		}

		/* The terminator on. The frame owes a third, empty packet, so it is
		 * not completed when its last byte is across. */
		{
			g2::BoardConfig cfg = base;
			cfg.usbTerminateWithZeroLengthPacket = true;

			g2::Board on(cfg);
			g2::InternalClient client(on.transport(), 4096, 4);

			check(client.send(g2::ProtocolFrame{ exact.data(), exact.size() }),
				"the exact-multiple frame is originated with the terminator on");

			on.tickSofIfDue(0);
			const g2::Board::UsbTransportStats u = on.usbTransport();

			checkEqual(u.accepted,   1u, "the first of three packets is taken");
			checkEqual(u.heldOffset, packet, "the cursor sits one packet in");
			checkEqual(u.completed,  0u,
				"a frame that still owes its terminator is not completed");
			check(u.held, "the frame is still held for its remaining packets");
		}

		/* The flag changes something, and a short frame is unaffected by it.
		 * A terminator applied to every frame would be the plausible wrong
		 * implementation, and it would look identical on the case above. */
		{
			g2::BoardConfig cfg = base;
			cfg.usbTerminateWithZeroLengthPacket = true;

			g2::Board on(cfg);
			g2::InternalClient client(on.transport(), 4096, 4);

			uint8_t shortFrame[3];
			fillPattern(shortFrame, sizeof(shortFrame), 9u);

			check(client.send(g2::ProtocolFrame{ shortFrame, sizeof(shortFrame) }),
				"a SHORT frame is originated with the terminator on");

			on.tickSofIfDue(0);
			const g2::Board::UsbTransportStats u = on.usbTransport();

			checkEqual(u.accepted,  1u, "the short frame crosses in one packet");
			checkEqual(u.completed, 1u,
				"a frame that is ALREADY short owes no terminator and is"
				" completed by its own last packet");
			check(!u.held, "nothing is held after a short frame completes");
		}
	}

	if(failures != 0)
	{
		printf("t0_board_transport: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_board_transport: all cases passed\n");
	return 0;
}
