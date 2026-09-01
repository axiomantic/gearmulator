// THROWAWAY INSTRUMENT (not registered in tests_board.cmake, not committed).
// Task: W3-454 clause (h), the one remaining runtime question -- the firmware's
// verdict on a real patch delivery, measured at the message worker 0x3004C10C.
//
// The route is t1_gdb_dsp's: GdbStub served in-process over loopback, driven by
// the test's own client thread via ask(). Delivery follows t1_patch_running:
// pch2Load -> InternalClient -> board.pumpTransport, on a BOOTED machine.
// Breakpoints are counted across many stops rather than single-shot: after
// each stop the instrument re-arms the same points and continues again, so a
// count is a measurement and a single-stop session is not.

#include "gatedFixture.h"

#include "../artifactResolver.h"
#include "../board.h"
#include "../crc16.h"
#include "../gdbStub.h"
#include "../internalClient.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"
#include "../transportHub.h"
#include "../../g2JucePlugin/g2PatchLoad.h"

#include "dsp56kBase/logging.h"

#include <array>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		std::cout << (_condition ? "ok   " : "FAIL ") << _what << std::endl;
		if(!_condition)
			++g_failures;
	}

	std::string hex32(const uint32_t _value)
	{
		char buf[11];
		std::snprintf(buf, sizeof buf, "0x%08x", unsigned(_value));
		return buf;
	}

	// THE 0x65 MORPH-PARAMETERS BIT TRANSFORM. Byte-for-byte the rule of
	// wire_compose.py's _morph_payload_tenth_variation: decode the g2fx
	// MorphParameters layout (variation count 8 bits, morph count 4, twenty
	// reserved, then per variation the variation index 4 bits, three reserved
	// fields 24/24/8, the variation's morph count 8, that many 29-bit
	// parameters (2+8+7+4+8) and a 4-bit tail), re-emit with the count at 10
	// and the LAST variation appended as the tenth, pad to a byte. Any
	// inexactness -- a payload that ends inside the layout, or holds bytes
	// beyond it -- returns an empty vector and the caller falls back to the
	// filler form, exactly where wire_compose raises ValueError.
	struct BitReader
	{
		const uint8_t* data;
		size_t bitLength;
		size_t pos = 0;

		uint32_t get(size_t _count)
		{
			uint32_t value = 0;
			for(size_t i = 0; i < _count; ++i)
			{
				value = (value << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1);
				++pos;
			}
			return value;
		}
	};

	struct BitWriter
	{
		std::vector<uint8_t> buf;
		size_t pos = 0;

		void put(size_t _count, uint32_t _value)
		{
			for(size_t shift = _count; shift-- > 0;)
			{
				if(pos % 8 == 0)
					buf.push_back(0);
				if((_value >> shift) & 1)
					buf[pos / 8] |= static_cast<uint8_t>(0x80 >> (pos % 8));
				++pos;
			}
		}

		void padToByte()
		{
			while(pos % 8)
				++pos;
		}
	};

	std::vector<uint8_t> morphWirePayload(const uint8_t* _begin, const uint8_t* _end)
	{
		const size_t bitLength = static_cast<size_t>(_end - _begin) * 8;
		if(bitLength < 32 + 8 * 275 + 72)
			return {};
		BitReader reader{_begin, bitLength};
		reader.get(8); // file variation count, already known to be 9
		const uint32_t morphCount = reader.get(4);
		const uint32_t reserved   = reader.get(20);
		struct Variation
		{
			uint32_t index, reserved0, reserved1, reserved2, morphCount;
			std::vector<std::array<uint32_t, 5>> params;
			uint32_t tail;
		};
		std::vector<Variation> variations;
		for(size_t v = 0; v < 9; ++v)
		{
			if(reader.pos + 72 > bitLength)
				return {};
			Variation var;
			var.index      = reader.get(4);
			var.reserved0  = reader.get(24);
			var.reserved1  = reader.get(24);
			var.reserved2  = reader.get(8);
			var.morphCount = reader.get(8);
			if(reader.pos + var.morphCount * 29 + 4 > bitLength)
				return {};
			for(size_t p = 0; p < var.morphCount; ++p)
			{
				std::array<uint32_t, 5> param = {
					reader.get(2), reader.get(8), reader.get(7), reader.get(4), reader.get(8)};
				var.params.push_back(param);
			}
			var.tail = reader.get(4);
			variations.push_back(std::move(var));
		}
		if(reader.pos != bitLength)
			return {};
		variations.push_back(variations.back());
		BitWriter writer;
		writer.put(8, 10);
		writer.put(4, morphCount);
		writer.put(20, reserved);
		for(const auto& var : variations)
		{
			writer.put(4, var.index);
			writer.put(24, var.reserved0);
			writer.put(24, var.reserved1);
			writer.put(8, var.reserved2);
			writer.put(8, var.morphCount);
			for(const auto& param : var.params)
			{
				writer.put(2, param[0]);
				writer.put(8, param[1]);
				writer.put(7, param[2]);
				writer.put(4, param[3]);
				writer.put(8, param[4]);
			}
			writer.put(4, var.tail);
		}
		writer.padToByte();
		return writer.buf;
	}

	std::string hexAddr(const uint32_t _value)
	{
		char buf[9];
		std::snprintf(buf, sizeof buf, "%x", unsigned(_value));
		return buf;
	}

	// --------------------------------------------------- the chain's addresses
	// All from plan W3-453/W3-454, none invented here.
	constexpr uint32_t g_isr         = 0x30053C38u;
	constexpr uint32_t g_ep3Callback = 0x30055D36u;
	constexpr uint32_t g_completion  = 0x30055DD0u;
	constexpr uint32_t g_worker      = 0x3004C10Cu;
	constexpr uint32_t g_rearm       = 0x30055D62u;
	// THE WORKER'S VERDICT PROBES (v14, all Ghidra-derived). The worker
	// returns its status in D2; the join at 0x3004C1E2 moves D2 into D0 and
	// the jump table at 0x3004C1F6 (PC-relative) routes status 0..4 to
	// 0x30050C2F / 0x3004F21C / 0x30053BF8 / 0x300527FC / 0x300531F7. The
	// decompiler mis-called the 0x25 path: the real dispatcher jump table at
	// 0x30012072 sends type 0x25 to 0x3001DF24 (a UI refresh), NOT to
	// 0x30013568/FUN_3004C10C. The worker's caller is 0x30039B96
	// (slot-2 route of FUN_30039B96) and the type-6 mark at slot -1 is where
	// it turns the claimed slot into the worker call.
	// THE JOIN, TWO PROBES (v34, after the v33 run's ambiguity). The join
	// block, disassembled from CODE_30000400.bin:
	//   0x3004C1D4  move.b d0,d2   (the old probe: d2's low byte copies the
	//                               status, but the stub's stop can land with
	//                               1E0's clr.l d0 already retired, so a d0
	//                               read there is meaningless -- measured:
	//                               d0=0 while d2 low = 0xbd)
	//   0x3004C1D6  addq.l #8,a7
	//   0x3004C1D8  bra.s 0x3004C1E0
	//   0x3004C1DA  moveq #1,d2    (status-1 tail)  /  1DC bra.s 1E0
	//   0x3004C1DE  moveq #2,d2    (status-2 tail)
	//   0x3004C1E0  clr.l d0
	//   0x3004C1E2  move.b d2,d0   <- the status byte lands in d0 HERE
	//   0x3004C1E4  moveq #4,d1 ... cmp.l d0,d1 ... dispatch
	// THE JOIN POST is 0x3004C1E4: retired past move.b d2,d0, so d0 IS the
	// status byte the worker returned, read once, no ambiguity.
	constexpr uint32_t g_switchJoin  = 0x3004C1D4u; // kept: the pre-join probe
	constexpr uint32_t g_switchPost  = 0x3004C1E4u; // the verdict read point
	// THE 0x01 DISPATCH ARM (W3-454 (e) / Ghidra msg_0x01 @ 0x3004c30e): the
	// slot/channel message handler the worker's first-byte switch routes a
	// real 0x37 patch-load to. Its body reads the command byte at buf+3
	// ('7' = O_CREATE), the name copy at buf+7, and lands the object chain
	// into the assembler buffer through FUN_30030094.
	constexpr uint32_t g_dispatch01  = 0x3004C30Eu;
	// THE REAL WORKER CALL SITE: FUN_30039B96's slot-2 loop calls the handler
	// INDIRECTLY -- 0x30039AA4 `jsr (A0)` with A0 = *(0x302a7c74 + D3*32 +0xC)
	// -- and there is no direct jsr to 0x3004C10C anywhere in the image. The
	// 39a90 block right before it (move.b D7,D2 / A0=D2 / A0 = A0 + D2*2 /
	// A0 = *(A4 + D0*32 + 0xC)) is the table walk that loads A0.
	constexpr uint32_t g_handlerCall = 0x30013582u; // the RETIREMENT PC of the type-0x25 handler's `jsr 0x3004c10c` (0x3001357c): when the PC stands here, the worker has been CALLED with (buf,len) from the event. Post-instruction compares make the jsr's own address blind.
	constexpr uint32_t g_st1         = 0x3004C2DEu; // the status-1/2 body (jsr FUN_3001c682, clear state, report 1)
	constexpr uint32_t g_st2         = 0x3004C200u; // the status-3/4 body (error UI path)
	// THE POST-BOOT KNOWN POSITIVE: the main loop W3-454 (a) names, which runs
	// on every ordinary quantum after the banner settles.
	constexpr uint32_t g_mainLoop    = 0x30004674u;
	// ISR ENTRY + 2: the stub's breakpoint compare runs AFTER each instruction,
	// so a breakpoint on an exception-ENTRY address is structurally blind -- the
	// compare reads the PC only once the entry instruction has retired and the
	// PC has advanced past it. The second word of the ISR is where the PC
	// stands after the first instruction of a REALLY-ENTERED ISR, so this is
	// the runnable probe for the exception path the entry address cannot be.
	constexpr uint32_t g_isrPlus2    = 0x30053C3Au;
	// The pipe reader the ep3 callback calls (W3-453 (3): 0x300556FE), jsr-
	// reachable and therefore visible to the entry compare, and the two probes
	// inside it t1_patch_running's scaffold named: the copied-count site and
	// the completion compare.
	constexpr uint32_t g_pipeReader  = 0x300556FEu;
	constexpr uint32_t g_rxCompare   = 0x30055772u;
	// The known positive: the ISR entry, which t1_usb_isr measured firing on a
	// delivered ep3 packet. Boot-time positive: the pre-handshake routine
	// t1_gdb_dsp measured hit during an ordinary boot.
	constexpr uint32_t g_bpBootPositive = 0x300391E8u;
	// The queue the completion enqueues event 0x25 into; the struct pointer is
	// AT 0x302A26F8, layout +0 tail, +2 head (plan W3-454 (b)).
	constexpr uint32_t g_queueStruct = 0x302A26F8u;

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;
	constexpr int g_regPc  = 17;
	constexpr int g_regVbr = 18;

	constexpr uint32_t g_vectorTableBase    = 0x30000000u;
	constexpr uint32_t g_vectorTableEntries = 256u;
	constexpr uint32_t g_vectorHandler      = 0x300585CEu;

	constexpr uint32_t g_mbarBase  = 0x10000000u;
	constexpr uint32_t g_cs0Base   = 0x00000000u;
	constexpr uint32_t g_cs0Size   = 0x00020000u;
	constexpr uint32_t g_cs1Size   = 0x00010000u;
	constexpr uint32_t g_cs2Base   = 0x12000000u;
	constexpr uint32_t g_cs2Size   = 0x00800000u;
	constexpr uint32_t g_cs3Size   = 0x00010000u;
	constexpr uint32_t g_cs4Base   = 0x14000000u;
	constexpr uint32_t g_cs4Size   = 0x00010000u;
	constexpr uint32_t g_cs5Size   = 0x00000010u;
	constexpr uint32_t g_sdramSize = 0x00800000u;

	constexpr uint32_t g_bootQuantumBound   = 500000u;
	constexpr uint32_t g_bannerSettleQuanta = 20000u;
	constexpr uint32_t g_displayBase        = 0x302A0DB8u;
	constexpr uint32_t g_lineWidth          = 16u;

	// Log filter, INT-1's, as t1_gdb_dsp installs it.
	const char* const g_underrunMessage = "ESAI transmit underrun";
	constexpr uint64_t g_underrunLinesKept = 4;
	std::atomic<uint64_t> g_underrunLines{0};

	void filterLog(const std::string& _s)
	{
		if(_s.find(g_underrunMessage) != std::string::npos &&
		   g_underrunLines.fetch_add(1) >= g_underrunLinesKept)
			return;
		std::cout << _s << '\n';
	}

	void installLogFilter()
	{
		if(std::getenv("G2_LOG_ESAI_UNDERRUN"))
			return;
		Logging::setLogFunc(&filterLog);
	}

	// A word-addressed SDRAM backing store with a peek that bypasses nothing:
	// reads go through the same bytes the core reads.
	//
	// THE SECOND ROLE (v32): when constructed with a nonzero translate base,
	// the same class backs the fixture's CS4 window, which this instrument
	// stretches over BOTH the panel hole at 0x14000000 (whose accesses the
	// boot's early path needs answered -- a Panel-sized zero-fill) and the
	// SRAM banks at 0x20000000. Window-relative offsets at or above the
	// translate base address the m_bytes array at (offset - translate); the
	// hole below it answers zero read and drops writes, exactly what the
	// out-of-range path of the plain Ram already did for the Panel.
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size, const uint32_t _translate = 0u)
			: m_bytes(_size, 0u), m_translate(_translate)
		{
		}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}
			if(_offset < m_translate)
				return 0u;
			const uint32_t base = _offset - m_translate;
			const uint32_t count = uint32_t(_size) / 8u;
			uint32_t value = 0u;
			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(base) + i;
				value = (value << 8) | (index < m_bytes.size() ? m_bytes[index] : 0u);
			}
			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value,
			mcf5307_bus_status& _status) override
		{
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}
			if(_offset < m_translate)
				return;
			const uint32_t base = _offset - m_translate;
			const uint32_t count = uint32_t(_size) / 8u;
			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(base) + i;
				if(index >= m_bytes.size())
					continue;
				const int shift = int(8u * (count - 1u - i));
				m_bytes[index] = uint8_t((_value >> shift) & 0xffu);
			}
		}

		bool place(const uint32_t _offset, const std::vector<uint8_t>& _image)
		{
			if(size_t(_offset) + _image.size() > m_bytes.size())
				return false;
			std::memcpy(m_bytes.data() + _offset, _image.data(), _image.size());
			return true;
		}

		uint32_t peekLong(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0u;
			const size_t off = size_t(_absolute - g2::g_sdramBase);
			if(off + 4u > m_bytes.size())
				return 0u;
			return (uint32_t(m_bytes[off]) << 24) | (uint32_t(m_bytes[off + 1]) << 16)
				| (uint32_t(m_bytes[off + 2]) << 8) | uint32_t(m_bytes[off + 3]);
		}

		uint32_t peekByte(const uint32_t _absolute) const
		{
			return peekWord(_absolute & ~1u) >> ((~_absolute & 1u) * 8u) & 0xffu;
		}

		uint32_t peekWord(const uint32_t _absolute) const
		{
			if(_absolute < g2::g_sdramBase)
				return 0u;
			const size_t off = size_t(_absolute - g2::g_sdramBase);
			if(off + 2u > m_bytes.size())
				return 0u;
			return (uint32_t(m_bytes[off]) << 8) | uint32_t(m_bytes[off + 1]);
		}

	private:
		std::vector<uint8_t> m_bytes;
		uint32_t m_translate;
	};

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		if(!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	// THE SRAM WINDOW. The loader's hw_init writes zeros over 0x20000000..0x20000FFF
	// and decompresses SRAM_20000800.bin into the second 4 KiB bank there; the
	// message worker's accept path executes code from that bank (the fault PC
	// 0x20000F48 sits at image offset 0x748, on a LINK prologue -- real code).
	// No authority records the window's full extent; the impl plan's fixed 4 KiB
	// per-bank mapping covers the boot path, and this fixture maps 64 KiB so the
	// assembler has room. Values 0x20000181 / 0x20000981 say RAMBAR0/1 carried
	// these banks with WP set; the core models no write-protect, so the window
	// is writable here.
	//
	// THE CS4 SPAN. The boot's early path touches the panel hole at 0x14000000
	// (measured 2026-08-30: unmapping it hung the boot in the vector handler
	// with A0=0x14000000), so the CS4 window here stretches from 0x14000000
	// over BOTH holes, and the sram Ram translates offsets at or above
	// g_sramTranslate (0x20000000 - 0x14000000 = 0xC0000000) into its bytes.
	// The panel hole below it answers zero read and dropped write, the same
	// shape the Panel's out-of-range path gave the boot before v32 remapped
	// CS4.
	constexpr uint32_t g_sramBase = 0x20000000u;
	constexpr uint32_t g_sramSize = 0x00010000u;
	constexpr uint32_t g_sramImageOffset = 0x800u;
	constexpr uint32_t g_cs4SpanBase = 0x14000000u;
	// Size ends AT the SDRAM base so the span never claims 0x30000000 (the
	// decode's first-match order puts Cs4 before Sdram, and an oversized span
	// faults every SDRAM access -- measured 2026-08-30, boot dead at quanta=1).
	// The span therefore also swallows the CS5 latch hole at 0x15000000; the
	// sram Ram answers that hole zero/OK like the panel hole, which changes
	// the panel latch bits the banner would read and nothing the boot check
	// asserts (the display cells live in SDRAM).
	constexpr uint32_t g_cs4SpanSize = g2::g_sdramBase - g_cs4SpanBase;
	constexpr uint32_t g_sramTranslate = g_sramBase - g_cs4SpanBase;

	// Where the image comes from. The artifact directory carries it beside
	// CODE_30000400.bin (FINDINGS.md TOOL-4), resolved the same way.
	const char* const g_sramImageName = "SRAM_20000800.bin";

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;
		config.memory.cs0   = {g_cs0Base,       g_cs0Size};
		config.memory.cs1   = {g2::g_cs1Base,   g_cs1Size};
		config.memory.cs2   = {g_cs2Base,       g_cs2Size};
		config.memory.cs3   = {g2::g_cs3Base,   g_cs3Size};
		config.memory.cs4   = {g_cs4Base,       g_cs4Size};
		config.memory.cs5   = {g2::g_cs5Base,   g_cs5Size};
		config.memory.mbar  = {g_mbarBase,      g2::g_simSpaceSize};
		config.memory.sdram = {g2::g_sdramBase, g_sdramSize};
		// THE SRAM WINDOW rides the CS4 slot as a stretched span over BOTH the
		// panel hole (0x14000000, which the boot's early path touches) and the
		// SRAM banks (0x20000000, which the accept path executes from); the
		// sram Ram attached below translates between them. The decode's
		// first-match order never reaches CS4 for any address CS0..CS3 or MBAR
		// or SDRAM claims.
		config.memory.cs4   = {g_cs4SpanBase,   g_cs4SpanSize};
		return config;
	}

	// ---------------------------------------------- the RSP client (t1_gdb_dsp's)

	std::string framed(const std::string& _payload)
	{
		unsigned sum = 0;
		for(const char c : _payload)
			sum += unsigned(uint8_t(c));
		char tail[4];
		std::snprintf(tail, sizeof tail, "#%02x", sum & 0xffu);
		return "$" + _payload + tail;
	}

	class Client final
	{
	public:
		bool connect(const uint16_t _port)
		{
			m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
			if(m_fd < 0)
				return false;
			sockaddr_in addr{};
			addr.sin_family      = AF_INET;
			addr.sin_port        = htons(_port);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if(::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
			{
				::close(m_fd);
				m_fd = -1;
				return false;
			}
			int one = 1;
			::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
			return true;
		}

		void close()
		{
			if(m_fd >= 0)
				::close(m_fd);
			m_fd = -1;
		}

		std::string exchange(const std::string& _payload)
		{
			const std::string out = framed(_payload);
			if(::send(m_fd, out.data(), out.size(), 0) != ssize_t(out.size()))
				return "<send failed>";
			char ack = 0;
			if(::recv(m_fd, &ack, 1, 0) != 1)
				return "<no ack>";
			if(ack != '+')
				return std::string("<nak ") + ack + ">";
			std::string payload;
			char c = 0;
			while(::recv(m_fd, &c, 1, 0) == 1 && c != '$') {}
			if(c != '$')
				return "<no packet>";
			while(::recv(m_fd, &c, 1, 0) == 1 && c != '#')
				payload += c;
			if(c != '#')
				return "<unterminated packet>";
			char sum[2] = {0, 0};
			if(::recv(m_fd, &sum[0], 1, 0) != 1 || ::recv(m_fd, &sum[1], 1, 0) != 1)
				return "<no checksum>";
			unsigned expected = 0;
			for(const char ch : payload)
				expected += unsigned(uint8_t(ch));
			char want[3];
			std::snprintf(want, sizeof want, "%02x", expected & 0xffu);
			if(want[0] != sum[0] || want[1] != sum[1])
				return "<bad checksum>";
			const char plus = '+';
			if(::send(m_fd, &plus, 1, 0) != 1)
				return "<ack failed>";
			return payload;
		}

	private:
		int m_fd = -1;
	};

	struct Channel
	{
		std::mutex              mutex;
		std::condition_variable cv;
		std::string             request;
		std::string             reply;
		bool                    hasRequest = false;
		bool                    hasReply   = false;
		bool                    quit       = false;
	};

	void clientLoop(Channel& _channel, Client& _client)
	{
		for(;;)
		{
			std::string request;
			{
				std::unique_lock<std::mutex> lock(_channel.mutex);
				_channel.cv.wait(lock, [&] { return _channel.hasRequest || _channel.quit; });
				if(_channel.quit)
					return;
				request             = _channel.request;
				_channel.hasRequest = false;
			}
			const std::string reply = _client.exchange(request);
			{
				std::lock_guard<std::mutex> lock(_channel.mutex);
				_channel.reply    = reply;
				_channel.hasReply = true;
			}
			_channel.cv.notify_all();
		}
	}

	Channel*     g_channel = nullptr;
	g2::GdbStub* g_stub    = nullptr;

	std::string ask(const std::string& _payload)
	{
		{
			std::lock_guard<std::mutex> lock(g_channel->mutex);
			g_channel->request    = _payload;
			g_channel->hasRequest = true;
			g_channel->hasReply   = false;
		}
		g_channel->cv.notify_all();
		if(!g_stub->servePacket())
			return "<stub served nothing>";
		std::unique_lock<std::mutex> lock(g_channel->mutex);
		g_channel->cv.wait(lock, [&] { return g_channel->hasReply; });
		g_channel->hasReply = false;
		return g_channel->reply;
	}

	// The eighteen registers, big-endian hex, in the stub's own order.
	bool parseRegisters(const std::string& _reply, uint32_t (&_regs)[18])
	{
		if(_reply.size() < 18u * 8u)
			return false;
		for(int i = 0; i < 18; ++i)
			_regs[i] = uint32_t(std::strtoul(_reply.substr(size_t(i) * 8u, 8u).c_str(), nullptr, 16));
		return true;
	}

	// One m read through the session. Returns an empty vector on refusal.
	std::vector<uint8_t> readMem(const uint32_t _addr, const uint32_t _len)
	{
		const std::string reply = ask("m" + hexAddr(_addr) + "," + hexAddr(_len));
		if(reply.size() != _len * 2u)
			return {};
		std::vector<uint8_t> out;
		for(uint32_t i = 0; i < _len; ++i)
			out.push_back(uint8_t(std::strtoul(reply.substr(i * 2u, 2u).c_str(), nullptr, 16)));
		return out;
	}

	// ------------------------------------------------------------ the session

	struct Session
	{
		Channel   channel;
		Client    client;
		uint16_t  port = 0;
		bool      connected = false;
		bool      accepted  = false;
	};

	bool openSession(Session& _s, g2::GdbStub& _stub)
	{
		_s.port = _stub.listenOn(0);
		if(_s.port == 0)
			return false;
		std::thread connector([&] { _s.connected = _s.client.connect(_s.port); });
		_s.accepted = _stub.waitForClient();
		connector.join();
		return _s.connected && _s.accepted;
	}

	void closeSession(Session& _s)
	{
		{
			std::lock_guard<std::mutex> lock(_s.channel.mutex);
			_s.channel.quit = true;
		}
		_s.channel.cv.notify_all();
		_s.client.close();
	}

	// -------------------------------------------------------- the boot machine
	//
	// THE BOOT RUNS WITHOUT THE STUB'S SCHEDULER ATTACHMENT. t1_patch_running's
	// drive loop is the boot; the stub is attached AFTER boot so the session's
	// continue never has to cross the multi-second boot regime through a
	// one-quantum-at-a-time packet loop. The stub is constructed after every
	// unit is attached, which this order satisfies.

	struct Machine
	{
		g2::Board     board{makeConfig()};
		Ram           ram{g_sdramSize};
		// The translate puts the image's window-relative offset (0xC0000800,
		// i.e. 0x20000800 seen from the 0x14000000 span base) at index 0x800.
		Ram           sram{g_sramSize, g_sramTranslate};
	};

	bool body(const std::string& _directory)
	{
		const std::vector<uint8_t> code = readFile(_directory + "/CODE_30000400.bin");
		if(code.empty())
		{
			std::cout << "FAIL CODE_30000400.bin is empty or unreadable under " << _directory << std::endl;
			return false;
		}

		const std::string patchPath = _directory + "/" + G2_PATCH_RELATIVE_PATH;
		const std::vector<uint8_t> patch = readFile(patchPath);
		if(patch.empty())
		{
			std::cout << "FAIL the patch is empty or unreadable at " << patchPath << std::endl;
			return false;
		}
		std::cout << "patch: " << patchPath << " (" << patch.size() << " bytes)" << std::endl;

		Machine m;

		if(!m.ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "FAIL the image does not fit the configured SDRAM window" << std::endl;
			return false;
		}
		{
			std::vector<uint8_t> table(g_vectorTableEntries * 4u);
			for(uint32_t entry = 0; entry < g_vectorTableEntries; ++entry)
				for(uint32_t byte = 0; byte < 4u; ++byte)
					table[entry * 4u + byte] = uint8_t((g_vectorHandler >> ((3u - byte) * 8u)) & 0xffu);
			if(!m.ram.place(g_vectorTableBase - g2::g_sdramBase, table))
			{
				std::cout << "FAIL the vector table does not fit the SDRAM window" << std::endl;
				return false;
			}
		}

		m.board.memory().attach(g2::Region::Sdram, &m.ram);
		m.board.memory().attach(g2::Region::Cs4, &m.sram);

		// THE SRAM IMAGE, at 0x20000800 inside the translated bank; everything
		// else in the bank is the zero fill the loader's hw_init performs. The
		// place offset is BANK-relative: the translate already removed the
		// span base, so 0x20000800 lands at bank index 0x800.
		{
			const std::vector<uint8_t> sramImage = readFile(_directory + "/" + g_sramImageName);
			if(sramImage.empty())
			{
				std::cout << "FAIL SRAM_20000800.bin is empty or unreadable under "
					<< _directory << std::endl;
				return false;
			}
			std::cout << "sram image: " << g_sramImageName << " (" << sramImage.size()
				<< " bytes at 0x" << std::hex << (g_sramBase + g_sramImageOffset)
				<< std::dec << ")" << std::endl;
			if(!m.sram.place(g_sramImageOffset, sramImage))
			{
				std::cout << "FAIL the SRAM image does not fit the SRAM bank" << std::endl;
				return false;
			}
		}

		m.board.resetMcu(g_entrySp, g_entryPc);
		if(!m.board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL the core refused VBR at register index " << g_regVbr << std::endl;
			return false;
		}

		g2::SerialExecutor executor;
		g2::Status         schedulerStatus{};
		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(g2::Scheduler::Config(), executor, m.board, schedulerStatus);
		if(!scheduler)
		{
			std::cout << "FAIL Scheduler::create returned no object; g2::Status = "
				<< uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		unsigned dspCount = m.board.dspSet().dspCount();

		// ---------------------------------------------------------- the boot
		uint32_t bootQuanta = 0;
		uint32_t settle = 0;
		for(uint32_t i = 0; i < g_bootQuantumBound; ++i)
		{
			bootQuanta = i + 1;
			scheduler->runFrames(1);
			if(m.board.mcuHalted())
				break;
			if(m.ram.peekWord(g_displayBase + 0u) == 0u && m.ram.peekWord(g_displayBase + 2u) == 0u)
				continue;
			if(++settle < g_bannerSettleQuanta)
				continue;

			unsigned landed = 0;
			for(unsigned d = 0; d < dspCount; ++d)
			{
				const bool* const flag = m.board.dspSet().programLanded(d);
				if(flag != nullptr && *flag)
					++landed;
			}
			if(landed == dspCount)
				break;
		}

		bool booted = settle >= g_bannerSettleQuanta;

		unsigned landedNow = 0;
		for(unsigned d = 0; d < dspCount; ++d)
		{
			const bool* const flag = m.board.dspSet().programLanded(d);
			if(flag != nullptr && *flag)
				++landedNow;
		}

		std::cout << "boot: quanta=" << bootQuanta << " booted=" << (booted ? 1 : 0)
			<< " programsLanded=" << landedNow << "/" << dspCount
			<< " halted=" << (m.board.mcuHalted() ? 1 : 0)
			<< " faulted=" << (m.board.faulted() ? 1 : 0) << std::endl;

		check(booted, "the machine booted: the display was written and settled");
		check(landedNow == dspCount, "every DSP position took its program during the boot");

		// --------------------------------- the queue head BEFORE any delivery
		const uint32_t queuePtr  = m.ram.peekLong(g_queueStruct);
		const uint32_t tailPre   = queuePtr != 0u ? m.ram.peekWord(queuePtr + 0u) : 0u;
		const uint32_t headPre   = queuePtr != 0u ? m.ram.peekWord(queuePtr + 2u) : 0u;
		std::cout << "queue: struct@0x302A26F8 -> " << hex32(queuePtr)
			<< " tail=" << tailPre << " head=" << headPre
			<< " (layout +0 tail, +2 head per W3-454 (b))" << std::endl;
		check(queuePtr != 0u, "the queue struct pointer at 0x302A26F8 is non-null after boot");
		check(queuePtr >= g2::g_sdramBase && queuePtr + 6u < g2::g_sdramBase + g_sdramSize,
			"the queue pointer points inside the SDRAM window, so the peeks below read real words");

		// ------------------------------- the stub, attached AFTER the boot
		g2::GdbStub stub(m.board);
		g_stub = &stub;
		stub.attachScheduler(*scheduler);

		Session session;
		check(openSession(session, stub),
			"the session binds a loopback port and accepts the client");
		if(!session.connected || !session.accepted)
			return false;

		g_channel = &session.channel;
		std::thread clientThread(clientLoop, std::ref(session.channel), std::ref(session.client));

		check(ask("qSupported:multiprocess+") == "PacketSize=1000",
			"the session opens the way the script's first packet opens it");

		// ------------------------------- the delivery through pch2Load
		// THE COMPOSER (v30). Plan W3-457's measured framing rule, from two
		// agreeing sources (capture-008 validated byte-for-byte against g2fx's
		// own Usb.prepareSendBuffer, and the runtime store-sweep evidence):
		//
		//     [2-byte BE total][body][2-byte BE CRC-16/CCITT-XMODEM over body]
		//
		// total counts the WHOLE frame INCLUDING the 2 prefix bytes; the CRC
		// sits DIRECTLY after the body with no pad (the W3-456 pad-to-64 was
		// an instrument artifact -- the real wire's termination is the SHORT
		// LAST USB PACKET). The BODY is the 0x01/0x37 patch-load message:
		//
		//     [0x01][0x28][0x53][0x37][0x00 0x00 0x00][entry name][chain]
		//
		// M_CMD, S_SLOT_REQ+0, V_NEW_PATCH, O_CREATE, three zeros (present in
		// every capture; unexplained in g2fx too), then the EntryName field
		// (name chars + one 0x00 terminator, or exactly 16 chars with NO
		// terminator when the name fills the field), then the .pch2 object
		// chain with wire_compose.py's difference-1 transform (variation count
		// 9 -> 10 plus one appended byte on a 0x4D/0x65 opening with 9).
		// DIFFERENCE 2 DOES NOT FIRE: the capture-008 host-to-device chain
		// carries no 0x2D 0x00 pair anywhere (measured: no such pair in 861
		// body bytes), so none is appended.
		constexpr int g_wireMode = 1; // 1 = 0x01/0x37 message; 0 = old bare frames

		// THE CRC CONTROL (W3-457's status-2 question). g_crcControl 0 =
		// good CRC; 1 = the body is DELIVERED with its CRC inverted (both
		// bytes complemented) while the frame still carries the good total,
		// so the worker's CRC gate sees a mismatch and everything else about
		// the frame is identical. One run per mode; the instrument is invoked
		// twice and the D0 readings compared across runs.
		const int g_crcControl = []{
			const char* env = std::getenv("T1_VERDICT_CRC_CONTROL");
			return (env != nullptr && env[0] == '1') ? 1 : 0;
		}();

		// The real patch name from the corpus file's own name would need a
		// payload decoder; the delivered name is the capture-008 one, a
		// 16-character name that fills the field with NO terminator --
		// exactly the boundary form the captures pin.
		const char* const g_entryName = "g2fx-uprate-4mod";

		std::vector<uint8_t> wire;

		if(g_wireMode == 1)
		{
			// Locate the binary header exactly as pch2Load does: the first NUL
			// terminates the text header; binary header, objects and trailing
			// CRC follow it.
			std::size_t terminator = 0;
			while(terminator < patch.size() && patch[terminator] != 0)
				++terminator;
			if(terminator == patch.size() || patch.size() < terminator + 5)
			{
				std::cout << "FAIL the patch has no NUL-terminated text header or is too short" << std::endl;
				return false;
			}
			const std::size_t binaryHeader = terminator + 1;
			const std::size_t bodyEnd      = patch.size() - 2; // the trailing 2-byte CRC is file furniture

			// THE ENTRY NAME FIELD. ascii chars + one 0x00 terminator, or
			// exactly 16 chars with NO terminator when the name fills it.
			std::vector<uint8_t> nameField;
			{
				const std::string name(g_entryName);
				if(name.size() > 16)
				{
					std::cout << "FAIL the entry name exceeds 16 characters" << std::endl;
					return false;
				}
				nameField.assign(name.begin(), name.end());
				if(name.size() < 16)
					nameField.push_back(0x00);
			}

			// THE CHAIN. wire_compose.py's difference-1 transform only: the
			// variation count byte of a 0x4D/0x65 object goes 9 -> 10 and
			// gains one appended byte (the declared length grows by 1).
			std::vector<uint8_t> body;
			// The message header: M_CMD, S_SLOT_REQ+0, V_NEW_PATCH, O_CREATE,
			// three zeros.
			const uint8_t header[7] = {0x01, 0x28, 0x53, 0x37, 0x00, 0x00, 0x00};
			body.insert(body.end(), header, header + 7);
			body.insert(body.end(), nameField.begin(), nameField.end());
			std::size_t offset = binaryHeader + 2;
			while(bodyEnd - offset >= 3)
			{
				const uint8_t     type   = patch[offset];
				const std::size_t length = (static_cast<std::size_t>(patch[offset + 1]) << 8) | patch[offset + 2];
				if(offset + 3 + length > bodyEnd)
					break;
				body.push_back(type);
				body.push_back(patch[offset + 1]);
				body.push_back(patch[offset + 2]);
				if(type == 0x65 && length >= 1 && patch[offset + 3] == 9)
				{
					// THE 0x65 TENTH-VARIATION TRANSFORM (wire_compose.py's
					// _morph_payload_tenth_variation, kept identical here).
					// The firmware's 0x65 reader walks a continuous bit
					// stream whose tenth pass reads the payload's remaining
					// bytes as a variation; a filler byte let that pass eat
					// the NEXT chunk's bytes (measured 329 consumed against
					// a 292-byte frame). The wire form needs a FULL tenth
					// variation: the copy of the LAST one. Implemented as
					// the byte-exact equivalent: decode the g2fx
					// MorphParameters bit layout (header 8+4+20 bits, then
					// per variation 4+24+24+8+8 bits + that many 29-bit
					// parameters + a 4-bit tail), re-emit with count 10 and
					// the last variation appended, pad to a byte.
					const std::vector<uint8_t> wirePayload =
						morphWirePayload(patch.data() + offset + 3,
							patch.data() + offset + 3 + length);
					if(!wirePayload.empty())
					{
						body.insert(body.end(), wirePayload.begin(), wirePayload.end());
						const std::size_t payloadLen = wirePayload.size();
						body[body.size() - payloadLen - 2] = static_cast<uint8_t>(payloadLen >> 8);
						body[body.size() - payloadLen - 1] = static_cast<uint8_t>(payloadLen & 0xffu);
						offset += 3 + length;
						continue;
					}
					// FALLBACK: the payload does not decode exactly through
					// the layout (wire_compose raises ValueError there too):
					// the filler form, which changes nothing the firmware
					// walk could not already reach.
					body.push_back(10);
					body.insert(body.end(), patch.begin() + static_cast<long>(offset) + 4,
						patch.begin() + static_cast<long>(offset) + 3 + static_cast<long>(length));
					body.push_back(0);
					const std::size_t payloadLen = length + 1;
					body[body.size() - payloadLen - 2] = static_cast<uint8_t>(payloadLen >> 8);
					body[body.size() - payloadLen - 1] = static_cast<uint8_t>(payloadLen & 0xffu);
					offset += 3 + length;
					continue;
				}
				body.insert(body.end(), patch.begin() + static_cast<long>(offset) + 3,
					patch.begin() + static_cast<long>(offset) + 3 + static_cast<long>(length));
				offset += 3 + length;
			}

			// THE FRAME: [2-byte BE total][body][2-byte BE CRC], no pad.
			// total counts the WHOLE frame including the prefix (measured 865
			// on capture-008, a non-multiple of 64); termination on the real
			// wire is the SHORT LAST USB PACKET, and the transport's chunking
			// handles that, so nothing here pads.
			wire.push_back(0); // prefix placeholder, high byte
			wire.push_back(0); // prefix placeholder, low byte
			wire.insert(wire.end(), body.begin(), body.end());
			wire.push_back(0); // CRC placeholder, high byte
			wire.push_back(0); // CRC placeholder, low byte

			const std::size_t total = wire.size();
			wire[0] = static_cast<uint8_t>(total >> 8);
			wire[1] = static_cast<uint8_t>(total & 0xffu);
			std::cout << "composer: body=" << body.size() << " pad=0"
				<< " total=" << total << std::endl;

			// THE COMPOSED BODY'S HEAD, printed for the slot-diff measurement.
			{
				std::ostringstream wd;
				wd << "  body[0..23] len=" << body.size() << ":";
				for(std::size_t k = 0; k < 24u && k < body.size(); ++k)
					wd << ' ' << hex32(body[k]);
				std::cout << wd.str() << std::endl;
			}

			// THE CRC. crc16.cpp carries the firmware-validated PROTO-1
			// parameters (init 0, poly 0x1021, no final xor) and the worker
			// applies them through FUN_300089dc over the delivered remainder
			// minus its trailing 2 bytes: the body, directly.
			uint16_t crc = g2::crc16(wire.data() + 2, wire.size() - 4);
			if(g_crcControl == 1)
			{
				// THE CONTROL: deliver the SAME frame with the CRC inverted.
				// The body, the total and every other byte are unchanged --
				// only the checksum the worker recomputes at FUN_300089dc
				// will disagree with what the last two bytes carry.
				crc = static_cast<uint16_t>(~crc);
			}
			wire[wire.size() - 2] = static_cast<uint8_t>(crc >> 8);
			wire[wire.size() - 1] = static_cast<uint8_t>(crc & 0xffu);
			std::cout << "composer: crcControl=" << g_crcControl
				<< " crc=" << hex32(crc) << std::endl;
		}

		if(g_wireMode == 0)
		{
			wire.assign(patch.begin(), patch.end());
		}

		g2::InternalClient client(m.board.transport(), 4096, 4);
		bool sendOk = true;
		for(std::size_t off = 0; off < wire.size(); )
		{
			const std::size_t chunk = std::min<std::size_t>(wire.size() - off, 4096u);
			if(!client.send(g2::ProtocolFrame{ wire.data() + off, chunk }))
			{
				sendOk = false;
				break;
			}
			off += chunk;
		}
		const g2::Pch2LoadResult loadResult = sendOk ? g2::Pch2LoadResult::Loaded
			: g2::Pch2LoadResult::SendRefused;
		std::cout << "delivery: composer mode " << g_wireMode << " answered "
			<< g2::pch2LoadResultName(loadResult) << " (" << wire.size() << " wire bytes)" << std::endl;
		check(loadResult == g2::Pch2LoadResult::Loaded,
			"the composed wire stream leaves through the BOARD'S OWN hub");

		// The pump with NO MCU cycle after it, exactly t1_patch_running's order.
		m.board.pumpTransport();
		const g2::Board::UsbTransportStats afterPump = m.board.usbTransport();
		std::cout << "transport after pump: drained=" << afterPump.drained
			<< " accepted=" << afterPump.accepted << " refused=" << afterPump.refused
			<< " completed=" << afterPump.completed << " held=" << (afterPump.held ? 1 : 0) << std::endl;
		check(afterPump.drained > 0, "frames really left the hub at the pump, so the breakpoints below weigh a real delivery");

		// ------------------------------- ARM the breakpoints (v30, FEW and
		// NARROW). The v29-and-before protocol armed twelve points and both
		// control runs' windows closed before the verdict printed (W3-457's
		// open clause). The status-2 question needs only: the worker entry,
		// the 0x01 dispatch arm 0x3004C30E, and the switch join where the
		// status byte lands. The delivery's noisy chain (ISR, ep3, pipe,
		// compare, completion, main loop, the queue watchpoint) is left
		// UNARMED; the observe loop's quantum-end sampling still counts the
		// worker's footprint without stopping the machine for it.
		const bool armDispatch = (g_crcControl == 0);
		check(ask("Z0," + hexAddr(g_worker) + ",2") == "OK",
			"Z0 accepted at the message worker 0x3004C10C");
		if(armDispatch)
		{
			check(ask("Z0," + hexAddr(g_dispatch01) + ",2") == "OK",
				"Z0 accepted at the 0x01 dispatch arm 0x3004C30E");
			check(ask("Z0," + hexAddr(g_switchJoin) + ",2") == "OK",
				"Z0 accepted at the worker's switch join 0x3004C1D4");
			check(ask("Z0," + hexAddr(g_switchPost) + ",2") == "OK",
				"Z0 accepted at the join post 0x3004C1E4");
			// THE ACCEPT-PATH FOLLOW-UPS (v31). The morph-fixed runs walked
			// the re-arm at 0x30055D62 into these two functions and faulted in
			// the unmapped SRAM window; with the window mapped, breakpoints
			// here show the chain executing and how far it gets.
			check(ask("Z0,3002a3d0,2") == "OK",
				"Z0 accepted at the accept-path follow-up FUN_3002a3d0");
			check(ask("Z0,3001dd32,2") == "OK",
				"Z0 accepted at the accept-path follow-up FUN_3001dd32");
		}

		// THE QUEUE WATCHPOINT IS LEFT UNARMED in the v30 protocol. It fires
		// on every allocation machine-wide (the good-CRC run's first stop was
		// a 0x302f8990 write from FUN_30003890 at 0x3000394a, not our queue),
		// and a stop between the delivery and the worker would mask the very
		// dispatch the instrument exists to see. The quantum-end queue reads
		// below still measure producer/consumer motion without stopping.

		// ------------------------------------------------- the observe loop
		//
		// THE DELIVERY SETTLES IN ~300 QUANTA (v18's qmove measured the event
		// consumed at q=300), so 600 quanta cover it. Then THE RESUME CAPTURE
		// takes over: quantum-end PC sampling is structurally blind to a
		// mid-quantum breakpoint stop -- runQuantum re-invokes the runner for
		// the remaining cycle want and steps straight past the stop inside the
		// same quantum -- but the stub's own resume path stops AT the hit with
		// the PC still standing on it. The capture loop continues through the
		// noisy chain stops and stops the world on the first worker-chain hit.
		struct Hits { uint64_t isr = 0, ep3 = 0, completion = 0, worker = 0, mainLoop = 0,
		isrPlus2 = 0, pipeReader = 0, rxCompare = 0, switchJoin = 0, case12 = 0, case34 = 0; } hits = {};
		bool     switchJoinD0Recorded = false;
		uint32_t switchJoinD0 = 0xffffffffu;
		bool     switchPostD0Recorded = false;
		uint32_t switchPostD0 = 0xffffffffu;

		struct WorkerSnap
		{
			uint32_t regs[18] = {};
			std::vector<uint8_t> mem24;
			uint32_t firstByte = 0x100u;
		};
		WorkerSnap firstWorkerSnap;
		bool workerSnapTaken = false;

		// Per-counter register snapshots, one each, plus the log of the first
		// 40 stops by PC.
		uint32_t snapIsr[18] = {}, snapEp3[18] = {}, snapCompl[18] = {}, snapWorker[18] = {};
		bool     haveIsr = false, haveEp3 = false, haveCompl = false;

		std::vector<std::string> stopLog;
		uint32_t lastStopPc = 0;
		uint32_t tailNow = tailPre, headNow = headPre;
		uint32_t tailMax = tailPre, headMax = headPre;
		uint64_t watchStops = 0;
		uint32_t watchFirstWriterPc = 0;
		uint32_t lastQuantum = 0;
		// The queue's element array, read at runtime: FUN_30003890 allocates
		// head*12 + *(base+10), so base+10 holds the element array pointer.
		const uint32_t queueElemBase = queuePtr != 0u ? m.ram.peekLong(queuePtr + 10u) : 0u;
		uint32_t lastQueuedTail = 0xffffffffu;
		uint32_t lastSeenHead = 0xffffffffu;
		Ram& m_ramRef = m.ram;
		uint8_t lastWorkerState = 0xffu;

		// THE WINDOW, LONGER for the v30 protocol. Both v28/v29 control runs
		// closed before their verdict print at 600 quanta under twelve
		// breakpoints; with three points armed and the noisy chain un-armed,
		// 2200 quanta leaves the delivery, the worker run and the queue
		// drain all inside the window. The tenth-variation fix makes the
		// chain walk consume the WHOLE object chain instead of breaking at
		// the shifted chunk 11, so the worker's pre-join path lengthened:
		// 2200 quanta closed with the walk still inside FUN_3002a2f8 (measured
		// 2026-08-30, no switch-join stop), and 6000 covers the full walk.
		const uint32_t observeQuanta = 6000u;

		for(uint32_t q = 0; q < observeQuanta; ++q)
		{
			lastQuantum = q + 1;
			scheduler->runFrames(1);

			if(m.board.mcuHalted())
				break;

			const uint32_t pc = m.board.mcuReg(g_regPc);

			// The stub sets m_stop inside runMcuBudget; its public surface does
			// not expose it, so the hit is read off the PC the machine holds.
			// The compare is the stub's own equality and this repeats it.
			const bool atMain  = pc == g_mainLoop;
			const bool atIsr   = pc == g_isr;
			const bool atEp3   = pc == g_ep3Callback;
			const bool atCompl = pc == g_completion;
			const bool atWorker= pc == g_worker;
			const bool atIsrP2 = pc == g_isrPlus2;
			const bool atPipe  = pc == g_pipeReader;
			const bool atRxCmp = pc == g_rxCompare;

			// THE QUEUE IS READ EVERY QUANTUM, NOT ONLY AT NAMED STOPS. The
			// v11 run gated every peek behind the 8 named PCs; after the
			// completion no named PC matched again, so the queue was never
			// read again and its counters froze at pre-completion values --
			// a frozen reading produced by the sampling gate, not by the
			// machine. The producer/consumer words and a coarse PC histogram
			// are therefore unconditional below.
			{
				tailNow = m.ram.peekWord(queuePtr + 0u);
				headNow = m.ram.peekWord(queuePtr + 2u);
				if(tailNow > tailMax) tailMax = tailNow;
				if(headNow > headMax) headMax = headNow;
				if(tailNow != lastQueuedTail || headNow != lastSeenHead)
				{
					std::ostringstream m;
					m << "  qmove q=" << q << " pc=" << hex32(pc)
						<< " prod=" << tailNow << " cons=" << headNow;
					// THE CLAIMED ELEMENT'S BYTES. The consumer read
					// *(elemBase + (cons-1 & mask)*12): dump that slice when
					// the consumer moved, so the event's type byte is a
					// measurement and not an inference from the enqueue
					// decompile.
					if(headNow != lastSeenHead && headNow != 0xffffu && queueElemBase != 0u)
					{
						const uint32_t idx   = (headNow - 1u) & 0x0fu;
						const uint32_t slice = queueElemBase + idx * 12u;
						m << " elem[" << idx << "] [0]=" << hex32(m_ramRef.peekLong(slice))
							<< " [4]=" << hex32(m_ramRef.peekLong(slice + 4u));
					}
					lastQueuedTail = tailNow;
					lastSeenHead   = headNow;
					stopLog.push_back(m.str());
				}
				if(pc >= g_mainLoop && pc < g_mainLoop + 0x800u)
					++hits.mainLoop;
				if(pc == g_switchJoin)
				{
					++hits.switchJoin;
					if(!switchJoinD0Recorded)
					{
						switchJoinD0Recorded = true;
						switchJoinD0 = m.board.mcuReg(2);
						std::ostringstream s;
						s << "  switchjoin q=" << q << " d2=status=" << hex32(switchJoinD0)
							<< " a0(buf)=" << hex32(m.board.mcuReg(8));
						stopLog.push_back(s.str());
					}
				}
				if(pc == g_handlerCall) ++hits.worker;

				// THE WORKER'S FOOTPRINT, sampled every quantum. The worker
				// clears the state byte DAT_30267a02 on every verdict path,
				// and 0x30055d62's re-arm clears the descriptor's complete
				// byte at 0x30280cd4. A mid-quantum breakpoint stop is
				// stepped past by runQuantum's re-invocation, so quantum-end
				// PC sampling cannot see the worker; these two memory
				// footprints are what a completed worker leaves behind.
				const uint8_t stateByte = uint8_t(m.ram.peekWord(0x30267a02u) & 0xffu);
				const uint8_t complByte = uint8_t(m.ram.peekWord(0x30280cd4u) & 0xffu);
				if(stateByte != lastWorkerState)
				{
					std::ostringstream w;
					w << "  workerstate q=" << q << " 0x30267a02: "
						<< hex32(lastWorkerState) << " -> " << hex32(stateByte)
						<< " descComplete=" << hex32(complByte);
					stopLog.push_back(w.str());
					lastWorkerState = stateByte;
				}
				if(pc == g_st1) ++hits.case12;
				if(pc == g_st2) ++hits.case34;
			}

			lastStopPc = pc;

			if(atMain)   ++hits.mainLoop;
			if(atIsr)    ++hits.isr;
			if(atEp3)    ++hits.ep3;
			if(atCompl)  ++hits.completion;
			if(atWorker) ++hits.worker;
			if(atIsrP2)  ++hits.isrPlus2;
			if(atPipe)   ++hits.pipeReader;
			if(atRxCmp)  ++hits.rxCompare;

			// THE DESCRIPTOR DUMP. a0 at the compare holds R (0x30280cd4, the
			// descriptor t1_patch_running's scaffold already named). The unit is
			// ONE 18-BYTE DESCRIPTOR: +0 completion byte, +2 expected total,
			// +6 received count, +10 buffer pointer. First and last compare
			// stops only, so the dump cannot alter the counts it qualifies.
			if(atRxCmp && (hits.rxCompare == 1u || hits.rxCompare % 23u == 0u))
			{
				const uint32_t r = m.board.mcuReg(8);
				std::ostringstream d;
				d << "  desc @" << hex32(r)
				  << " complete=" << (m.ram.peekWord(r) & 0xffu)
				  << " expected=" << m.ram.peekLong(r + 2u)
				  << " received=" << m.ram.peekLong(r + 6u)
				  << " buf=" << hex32(m.ram.peekLong(r + 10u))
				  << " cmp-d0=" << hex32(m.board.mcuReg(0))
				  << " cmp-d1=" << hex32(m.board.mcuReg(1));
				stopLog.push_back(d.str());
			}

			if(atIsrP2 && !haveIsr)
		{
			for(int i = 0; i < 18; ++i) snapIsr[i] = m.board.mcuReg(i);
			haveIsr = true;
		}
		if(atIsr && !haveIsr)
			{
				for(int i = 0; i < 18; ++i) snapIsr[i] = m.board.mcuReg(i);
				haveIsr = true;
			}
			if(atEp3 && !haveEp3)
			{
				for(int i = 0; i < 18; ++i) snapEp3[i] = m.board.mcuReg(i);
				haveEp3 = true;
			}
			if(atCompl && !haveCompl)
			{
				for(int i = 0; i < 18; ++i) snapCompl[i] = m.board.mcuReg(i);
				haveCompl = true;
				// THE QUEUE STRUCT AT THE MOMENT OF THE FIRST COMPLETION.
				// FUN_30003890(q): slot = *(ushort*)(q+2)*12 + *(int*)(q+10);
				// the FULL branch returns the marker 0x3012ba8c and writes
				// NOTHING. Field dump plus the marker write probe distinguish
				// "event queued" from "allocator refused".
				std::ostringstream qs;
				qs << "  qstruct @" << hex32(queuePtr)
					<< " [+0]=" << m.ram.peekWord(queuePtr)
					<< " [+2]=" << m.ram.peekWord(queuePtr + 2u)
					<< " [+4]=" << hex32(m.ram.peekLong(queuePtr + 4u))
					<< " [+6]=" << hex32(m.ram.peekLong(queuePtr + 6u))
					<< " [+10]=" << hex32(m.ram.peekLong(queuePtr + 10u))
					<< " markerProbe=" << hex32(m.ram.peekLong(0x3012ba8cu));
				stopLog.push_back(qs.str());
			}
			if(atWorker && !workerSnapTaken)
			{
				for(int i = 0; i < 18; ++i) firstWorkerSnap.regs[i] = m.board.mcuReg(i);
				firstWorkerSnap.mem24 = readMem(firstWorkerSnap.regs[8], 24);
				if(!firstWorkerSnap.mem24.empty())
					firstWorkerSnap.firstByte = firstWorkerSnap.mem24[0];
				workerSnapTaken = true;
			}

			tailNow = m.ram.peekWord(queuePtr + 0u);
			headNow = m.ram.peekWord(queuePtr + 2u);
			if(tailNow > tailMax) tailMax = tailNow;
			if(headNow > headMax) headMax = headNow;

			// THE EVENT-QUEUE SLICE DUMP, once per distinct tail value. The
			// queue element is 12 bytes; the completion at 0x30055DD0 writes
			// [+0]=0x25, [+2]=len, [+4]=buf. The main-loop quantum check
			// (below) is what makes each tail a distinct event, so its slice
			// is only sampled once.
			if(tailNow != lastQueuedTail && queueElemBase != 0u)
			{
				lastQueuedTail = tailNow;
				const uint32_t slice = queueElemBase + (tailNow & 0x0fu) * 12u;
				std::ostringstream e;
				e << "  evslot tail=" << tailNow
					<< " [0]=" << hex32(m.ram.peekLong(slice))
					<< " [4]=" << hex32(m.ram.peekLong(slice + 4u))
					<< " [8]=" << hex32(m.ram.peekLong(slice + 8u));
				stopLog.push_back(e.str());
			}

			// THE MAIN LOOP IS EVERY QUANTUM'S KNOWN POSITIVE: rather than a
			// counted Z0 on a one-instruction window, the count below is the
			// number of quanta whose LAST retired PC stood inside the
			// supervisor main-loop function. The worker and the queue drain
			// both live in that function, so a healthy post-delivery run
			// scores high and the pre-delivery boot quanta (before the stub
			// exists) score nothing.
			if(pc >= g_mainLoop && pc < g_mainLoop + 0x800u)
				++hits.mainLoop;
			if(pc == g_worker)
				++hits.worker;

			if(stopLog.size() < 40)
			{
				std::ostringstream line;
				line << "stop q=" << q << " pc=" << hex32(pc)
					<< (atMain ? " mainLoop" : "")
					<< (atIsr ? " ISR" : "")
					<< (atEp3 ? " ep3cb" : "")
					<< (atCompl ? " completion" : "")
					<< (atWorker ? " WORKER" : "")
					<< (atIsrP2 ? " ISR+2" : "")
					<< (atPipe ? " pipeRd" : "")
					<< (atRxCmp ? " rxCompare" : "");
				stopLog.push_back(line.str());

				if(stopLog.size() <= 24)
				{
					std::ostringstream r;
					r << "  regs d0-d3:";
					for(int i = 0; i < 4; ++i) r << ' ' << hex32(m.board.mcuReg(i));
					r << "  a0-a3:";
					for(int i = 8; i < 12; ++i) r << ' ' << hex32(m.board.mcuReg(i));
					stopLog.push_back(r.str());
				}
			}
		}

		for(const auto& line : stopLog)
			std::cout << line << std::endl;

		// THE RESUME CAPTURE. Quantum-end PC sampling is structurally blind to
		// a mid-quantum breakpoint stop: runQuantum re-invokes the runner for
		// the remaining cycle want, and the machine steps straight past the
		// stop inside the same quantum. The stub's own resume path
		// (`c` -> driveQuanta) stops AT the hit and answers the stop reply
		// with the PC still standing on it. Continue through the delivery's
		// noisy stops (ep3 / pipe / compare / ISR), capture the FIRST hit on
		// the worker chain, and record the worker's registers there.
		bool capturedWorkerStop = false;
		uint32_t capturedPc = 0;
		uint32_t capturedD2 = 0xffffffffu;
		std::vector<uint8_t> capturedBuf;
		// THE WALL-CLOCK CAP ON THE CAPTURE LOOP. A stuck stop-restart storm
		// must self-terminate: 180 s of captures is far past any live
		// delivery settles, and past it the run gives up rather than spin.
		const auto captureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		for(int pass = 0; pass < 400; ++pass)
		{
			if(std::chrono::steady_clock::now() > captureDeadline)
			{
				std::cout << "capture: WALL-CLOCK DEADLINE hit at pass " << pass
					<< " -- the window self-terminated" << std::endl;
				break;
			}
			const std::string reply = ask("c");
			if(reply.find("T") != 0)
			{
				std::cout << "resume pass " << pass << ": reply=\"" << reply << "\"" << std::endl;
				break;
			}
			const uint32_t stopPc = m.board.mcuReg(g_regPc);

			if(stopPc == g_worker)
			{
				capturedWorkerStop = true;
				capturedPc = stopPc;
				for(int i = 0; i < 18; ++i) firstWorkerSnap.regs[i] = m.board.mcuReg(i);
				// THE ARGUMENTS ARE ON THE STACK, not in registers: the
				// wrapper pushed len then buf before the jsr, so at the
				// worker's entry 4(A7)=buf and 8(A7)=len.
				const uint32_t a7 = m.board.mcuReg(15);
				const uint32_t buf = m.ram.peekLong(a7 + 4u);
				const uint32_t len = m.ram.peekLong(a7 + 8u);
				capturedBuf = readMem(buf, std::min<uint32_t>(len, 64u));
				workerSnapTaken = true;
				// THE SLOT HEAD: the descriptor buffer's first 32 bytes,
				// so the prefix/body boundary is measured and not inferred.
				std::ostringstream slotDump;
				slotDump << "  slot[0..31] @" << hex32(m.ram.peekLong(0x30280cd4u + 10u)) << ":";
				for(uint32_t k = 0; k < 32u; ++k)
					slotDump << ' ' << hex32(m.ram.peekByte(0x30280cfcu + k));
				std::cout << slotDump.str() << std::endl;
				std::cout << "resume stop " << pass << ": pc=" << hex32(stopPc)
					<< " a7=" << hex32(a7)
					<< " buf=" << hex32(buf) << " len=" << len
					<< " firstByte="
					<< (capturedBuf.empty() ? std::string("none") : hex32(capturedBuf[0]))
					<< std::endl;
				// v30: the worker stop does NOT end the capture. The question
				// is what the dispatch arm and the switch join record AFTER
				// this entry, so the loop continues and logs every further
				// stop (0x3004C30E, 0x3004C1D4) with its registers.
				continue;
			}
			if(stopPc == g_dispatch01)
			{
				// THE 0x01 ARM'S ARGUMENTS AND HEAD. The arm receives the
				// message pointer in A0 -- but the CALLER passes it on the
				// stack too, and the register read at the RETIRED PC may be
				// past the point where the arm's own prologue moved A0.
				// Record BOTH: the register and the stack slot 4(A7), the
				// same shape the worker-entry branch reads.
				const uint32_t a0 = m.board.mcuReg(8);
				const uint32_t a7 = m.board.mcuReg(15);
				const uint32_t stackBuf = m.ram.peekLong(a7 + 4u);
				std::ostringstream d;
				d << "  dispatch01 pass=" << pass
					<< " a0=" << hex32(a0) << " a7=" << hex32(a7)
					<< " stackbuf=" << hex32(stackBuf)
					<< " stackmsg[0..7]:";
				const std::vector<uint8_t> head = readMem(stackBuf, 8);
				for(const uint8_t b : head)
					d << ' ' << hex32(b);
				std::cout << d.str() << std::endl;
				continue;
			}

			// THE OTHER CHAIN STOPS ARE LOGGED AND WALKED THROUGH. The noisy
			// delivery stops are un-armed in v30; the remaining stops are the
			// switch join, the two follow-ups, and whatever else the worker
			// crosses.
			if(!switchJoinD0Recorded && stopPc == g_switchJoin)
			{
				switchJoinD0Recorded = true;
				switchJoinD0 = m.board.mcuReg(2);
				std::cout << "  switchjoin pass=" << pass
					<< " d0=" << hex32(m.board.mcuReg(0))
					<< " d2=" << hex32(m.board.mcuReg(2)) << std::endl;
			}
			if(!switchPostD0Recorded && stopPc == g_switchPost)
			{
				// THE VERDICT READ. The post retired past move.b d2,d0
				// (0x3004C1E2), so d0's low byte IS the worker's status byte.
				switchPostD0Recorded = true;
				switchPostD0 = m.board.mcuReg(0);
				std::cout << "  switchpost pass=" << pass
					<< " d0=" << hex32(switchPostD0)
					<< " d1=" << hex32(m.board.mcuReg(1))
					<< " d2=" << hex32(m.board.mcuReg(2)) << std::endl;
			}
			if(stopPc == 0x3002A3D0u || stopPc == 0x3001DD32u)
			{
				// THE FOLLOW-UP STOP: registers at entry, so the run into the
				// SRAM window is a measurement and not an absence of faults.
				std::ostringstream f;
				f << "  followup pass=" << pass << " pc=" << hex32(stopPc);
				for(int i = 0; i < 4; ++i) f << " d" << i << '=' << hex32(m.board.mcuReg(i));
				f << " a0=" << hex32(m.board.mcuReg(8))
					<< " a1=" << hex32(m.board.mcuReg(9));
				std::cout << f.str() << std::endl;
			}
			if(pass < 8 || stopPc == g_handlerCall || stopPc == g_completion
				|| stopPc == g_switchJoin || stopPc == g_switchPost
				|| stopPc == 0x3002A3D0u || stopPc == 0x3001DD32u)
				std::cout << "resume stop " << pass << ": reply=\"" << reply
					<< "\" pc=" << hex32(stopPc) << std::endl;
		}
		std::cout << "capture: workerStop=" << (capturedWorkerStop ? 1 : 0)
			<< " pc=" << hex32(capturedPc)
			<< " switchJoinD0=" << hex32(switchJoinD0Recorded ? switchJoinD0 : 0xffffffffu)
			<< " firstByte="
			<< (capturedBuf.empty() ? std::string("none") : hex32(capturedBuf[0]))
			<< std::endl;

		// THE NAME COPY AND THE ASSEMBLER BUFFER, read AFTER the window: the
		// 0x01 arm copies the entry name to 0x30267a0a (copy_token16
		// FUN_30037d50) and lands the object chain into the stream buffer at
		// 0x302579f8 through apply_stream FUN_30030094. Reading both after
		// the window settles whether the arm consumed the delivery.
		{
			std::ostringstream nb;
			nb << "  namebuf@0x30267a0a:";
			const std::vector<uint8_t> nameBuf = readMem(0x30267a0au, 17);
			for(const uint8_t b : nameBuf)
				nb << ' ' << hex32(b);
			std::cout << nb.str() << std::endl;
			std::ostringstream ab;
			ab << "  streambuf@0x302579f8 [0..7]:";
			const std::vector<uint8_t> streamHead = readMem(0x302579f8u, 8);
			for(const uint8_t b : streamHead)
				ab << ' ' << hex32(b);
			// The stream buffer's layout (init_buffer FUN_300373c0): +4
			// u16 position, +6 u8 flag. The position after a consumed
			// chain is NONZERO; an init-only buffer reads 0.
			ab << " pos=" << m.ram.peekWord(0x302579f8u + 4u)
				<< " flag=" << hex32(m.ram.peekWord(0x302579f8u + 6u) & 0xffu);
			std::cout << ab.str() << std::endl;
		}

		std::cout << "hits: mainLoop=" << hits.mainLoop
			<< " isr=" << hits.isr
			<< " ep3cb=" << hits.ep3
			<< " completion=" << hits.completion
			<< " worker=" << hits.worker
			<< " switchJoin=" << hits.switchJoin
			<< " case12=" << hits.case12
			<< " case34=" << hits.case34
			<< " isrPlus2=" << hits.isrPlus2
			<< " pipeRd=" << hits.pipeReader
			<< " rxCompare=" << hits.rxCompare
			<< " quanta=" << lastQuantum
			<< " tailPre=" << tailPre << " tailMax=" << tailMax
			<< " headPre=" << headPre << " headMax=" << headMax
			<< " lastStopPc=" << hex32(lastStopPc)
			<< " halted=" << (m.board.mcuHalted() ? 1 : 0)
			<< " faulted=" << (m.board.faulted() ? 1 : 0) << std::endl;

		const g2::Board::UsbTransportStats afterWindow = m.board.usbTransport();
		std::cout << "transport after window: drained=" << afterWindow.drained
			<< " accepted=" << afterWindow.accepted << " refused=" << afterWindow.refused
			<< " completed=" << afterWindow.completed
			<< " stallReports=" << afterWindow.stallReports
			<< " held=" << (afterWindow.held ? 1 : 0) << std::endl;

		// ------------------------------------------------- the verdicts
		std::cout << "verdict: mainLoopWindow=" << hits.mainLoop
			<< " quanta (retired-PC range probe; the counted Z0 on the one-instruction"
			<< " window is structurally near-blind and reported 0 in every run)" << std::endl;
		// THE ISR ENTRY COUNTER IS REPORTED AND NOT ASSERTED: the stub's
		// breakpoint compare runs after each instruction, so an exception-ENTRY
		// address can never read non-zero unless the handler is re-entered --
		// the figure is structural blindness, not an absence. ISR+2 is the
		// runnable probe.
		std::cout << "verdict: ISR entry 0x30053C38 counted " << hits.isr
			<< " (entry-compare blind by construction) and ISR+2 0x30053C3A counted "
			<< hits.isrPlus2 << std::endl;
		check(hits.isrPlus2 > 0,
			"the ISR really ENTERED on the delivered patch (ISR+2 probe); counted "
			+ std::to_string(hits.isrPlus2));
		check(hits.ep3 > 0,
			"the ep3 callback 0x30055D36 fired; counted " + std::to_string(hits.ep3));
		check(hits.completion > 0,
			"the completion enqueue 0x30055DD0 fired; counted " + std::to_string(hits.completion));
		check(hits.switchJoin > 0,
			"the worker's switch join 0x3004C1E0 was reached; counted "
			+ std::to_string(hits.switchJoin));
		check(headMax != headPre,
			"the queue HEAD advanced (pre=" + std::to_string(headPre) + " max=" + std::to_string(headMax) + ")");

		// THE WORKER'S VERDICT, from the join-post D0 (the authoritative read) and
		// the pre-join probes beside it. The disassembly names the mapping: the
		// status byte lands in d0 at 0x3004C1E2 (move.b d2,d0); 1/2 are the
		// moveq tails, 3/4 ride the same d0 path, and 0 is the accept path's
		// cleared d0.
		if(switchPostD0Recorded)
		{
			const uint8_t st = uint8_t(switchPostD0 & 0xffu);
			std::cout << "verdict: worker status byte (D0 at join post 0x3004C1E4) = " << hex32(st) << " -> ";
			if(st == 0)          std::cout << "STATUS 0 ACCEPT (and re-arm via 0x30055D62)";
			else if(st == 1)     std::cout << "STATUS 1: unknown message type";
			else if(st == 2)     std::cout << "STATUS 2: CRC mismatch";
			else if(st == 3)     std::cout << "STATUS 3: handler refused";
			else if(st == 4)     std::cout << "STATUS 4: busy/refused";
			else                 std::cout << "UNNAMED STATUS";
			std::cout << " (case12=" << hits.case12 << " case34=" << hits.case34 << ")" << std::endl;
		}
		else if(switchJoinD0Recorded)
		{
			const uint8_t st = uint8_t(switchJoinD0 & 0xffu);
			std::cout << "verdict: worker status byte (D2 low at pre-join 0x3004C1D4; the join-post probe never hit) = " << hex32(st) << " -> ";
			if(st == 0)          std::cout << "STATUS 0 ACCEPT (and re-arm via 0x30055D62)";
			else if(st == 1)     std::cout << "STATUS 1: unknown message type";
			else if(st == 2)     std::cout << "STATUS 2: CRC mismatch";
			else if(st == 3)     std::cout << "STATUS 3: handler refused";
			else if(st == 4)     std::cout << "STATUS 4: busy/refused";
			else                 std::cout << "UNNAMED STATUS";
			std::cout << " (case12=" << hits.case12 << " case34=" << hits.case34 << ")" << std::endl;
		}
		else
		{
			std::cout << "verdict: the switch join never recorded D0" << std::endl;
		}
		check(!m.board.faulted(), "the board reports no fault when the window closes");

		// ------------------------------------------------------- the detach
		check(ask("D") == "OK", "the stub answers the detach packet");
		closeSession(session);
		clientThread.join();
		stub.close();

		g_stub    = nullptr;
		g_channel = nullptr;

		return g_failures == 0;
	}
}

int main()
{
	installLogFilter();

	g2::EnvArtifactResolver resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]
	{
		std::string why;
		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");
		if(directory.empty())
		{
			std::cout << "FAIL t1_usb_verdict: " << why << std::endl;
			++g_failures;
			return false;
		}
		return body(directory);
	});

	std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
		<< (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;
	std::cout << g2::test::summaryLine(counters) << std::endl;
	return g2::test::gatedExitCode(counters);
}
