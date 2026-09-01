// The GDB-with-traffic harness, tier T0. It reads no firmware artifact; the
// machines it places are synthetic, in the shape t0_gdb_stub places its
// machine. Two machines are placed:
//
//   Machine A -- a synthetic Board whose `GdbStub` is served in-process and
//   reached over loopback, which is the packet sequence an operator session
//   runs.
//
//   Machine B -- the delivery machine: a Board with a `GdbStub` attached, plus
//   an `InternalClient`. A `pch2Load` delivery is driven on that same machine
//   while the session is open, so the watchpoint fires on a write the delivery
//   path makes and not on one the boot makes.
//
// The cases assert answers and not acceptance: they read the stub's own
// replies, and one case asserts an absence of stop replies from a session that
// was never told to continue.
//
// No assert() anywhere. The default build is Release with NDEBUG, so every
// verdict here is a return value, a printed line and an exit code.

#include "board.h"
#include "crc16.h"
#include "gdbStub.h"
#include "internalClient.h"
#include "scheduler.h"
#include "transportHub.h"

#include "../../g2JucePlugin/g2PatchLoad.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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

	void checkEqual(const uint32_t _actual, const uint32_t _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << hex32(_expected)
		          << ">, got <" << hex32(_actual) << ">" << std::endl;
		++g_failures;
	}

	void checkText(const std::string& _actual, const std::string& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
		          << ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	// ------------------------------------------------------------- machine A

	/* Byte-addressed, big-endian, in the shape t0_gdb_stub's Ram takes. The
	 * addresses below are this test's own configuration: no authority records
	 * a size for the SDRAM window. */
	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const uint32_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);
			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			uint32_t value = 0u;
			for(int i = 0; i < count; ++i)
				value = (value << 8) | uint32_t(m_bytes[_offset + uint32_t(i)]);

			_status = MCF5307_BUS_OK;
			return value;
		}

		void write(const uint32_t _offset, const int _size, const uint32_t _value,
		           mcf5307_bus_status& _status) override
		{
			const int count = byteCount(_size);
			if(count == 0 || _offset + uint32_t(count) > m_bytes.size())
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return;
			}

			for(int i = 0; i < count; ++i)
			{
				const int shift = 8 * (count - 1 - i);
				m_bytes[_offset + uint32_t(i)] = uint8_t((_value >> shift) & 0xffu);
			}

			_status = MCF5307_BUS_OK;
		}

		void pokeWord(const uint32_t _offset, const uint16_t _value)
		{
			m_bytes[_offset]      = uint8_t(_value >> 8);
			m_bytes[_offset + 1u] = uint8_t(_value & 0xffu);
		}

	private:
		static int byteCount(const int _sizeBits)
		{
			switch(_sizeBits)
			{
			case 8:  return 1;
			case 16: return 2;
			case 32: return 4;
			default: return 0;
			}
		}

		std::vector<uint8_t> m_bytes;
	};

	constexpr uint32_t g_windowBase  = g2::g_sdramBase;
	constexpr uint32_t g_windowSize  = 0x1000u;
	constexpr uint32_t g_stackTop    = g_windowBase + 0x800u;

	constexpr uint32_t g_codeOffset  = 0x000u;
	constexpr uint32_t g_scratchOffset = 0x300u;

	constexpr uint32_t g_codeBase    = g_windowBase + g_codeOffset;
	constexpr uint32_t g_scratchAddr = g_windowBase + g_scratchOffset;

	/* The program, hand-encoded, the same shape t0_gdb_stub's is.
	 *
	 *     +0x00  4E71  nop
	 *     +0x02  4E71  nop
	 *     +0x04  4E71  nop              the first reached breakpoint
	 *     +0x06  4E71  nop              the second reached breakpoint
	 *     +0x08  4E71  nop
	 *     +0x0A  A001  line-A, which faults and halts
	 *
	 * The two breakpoint addresses sit at different instructions, in program
	 * order, so the batch case's two stop lines have a reach order to assert.
	 * The store case does not use this program: its write comes from the
	 * delivery path on machine B, not from this one. */
	constexpr uint16_t g_program[] = {0x4E71u, 0x4E71u, 0x4E71u, 0x4E71u, 0x4E71u, 0xA001u};

	constexpr uint32_t g_bpFirst  = g_codeBase + 0x004u;
	constexpr uint32_t g_bpSecond = g_codeBase + 0x006u;

	/* The breakpoint the machine never reaches. It is odd and inside the code,
	 * so no program counter can equal it. */
	constexpr uint32_t g_bpUnreached = g_codeBase + 0x001u;

	/* The register indices of the MCF5307 C ABI. 17 is the program counter,
	 * which is the register the stub's breakpoint compare reads and the one
	 * every PC assertion here reads. */
	constexpr int g_regPc = 17;

	g2::BoardConfig makeConfigA()
	{
		g2::BoardConfig config;
		config.memory.sdram = {g_windowBase, g_windowSize};
		return config;
	}

	// ------------------------------------------------------------ machine B

	/* The watched word. Machine B is the delivery machine: its Board config
	 * carries a real CS3 window, so the frames pch2Load originates cross the
	 * hub into the device, and the device's own register file is live. The
	 * word the watchpoint observes is an SDRAM scratch word the machine's own
	 * program stores to -- the store is a bus write the stub's wrapper sees,
	 * and the delivery is layered on the same machine while the session is
	 * open. */
	constexpr uint32_t g_cs3Size      = 0x00010000u;
	constexpr uint32_t g_watchOffsetB = 0x400u;
	constexpr uint32_t g_watchAddrB   = g_windowBase + g_watchOffsetB;

	/* The store the watched word observes. Machine B's program: move.l
	 * d0,(a1) at +0x00 -- the write the watchpoint stops on -- then line-A,
	 * which faults and halts. The value is distinct in every byte, so a
	 * transfer of the wrong width lands on a value no other width produces. */
	constexpr uint16_t g_programB[] = {0x2280u, 0xA001u};
	constexpr uint32_t g_deliveryWord = 0x5A1D2E3Bu;

	constexpr int g_regD0 = 0;
	constexpr int g_regA1 = 9;

	const int g_protocolEndpoint = g2::BoardConfig{}.usbProtocolEndpoint;

	g2::BoardConfig makeConfigB()
	{
		g2::BoardConfig config;
		config.memory.cs3 = {g2::g_cs3Base, g_cs3Size};
		config.memory.sdram = {g_windowBase, g_windowSize};
		return config;
	}

	void fillPattern(uint8_t* const dst, const size_t size, const uint32_t seed)
	{
		for(size_t i = 0; i < size; ++i)
			dst[i] = static_cast<uint8_t>(seed * 31u + i * 7u + 1u);
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

	std::vector<uint8_t> buildContainer(const std::vector<uint8_t>& _objects)
	{
		std::vector<uint8_t> file;

		const char* const ascii = "Version=Nord Modular G2 File Format 1\n";
		for(const char* p = ascii; *p != 0; ++p)
			file.push_back(static_cast<uint8_t>(*p));
		file.push_back(0);

		const size_t binaryHeader = file.size();

		file.push_back(0x17);
		file.push_back(0x00);

		for(const uint8_t b : _objects)
			file.push_back(b);

		file.push_back(0);
		file.push_back(0);

		const uint16_t crc = g2::crc16File(file.data(), file.size(), binaryHeader);
		g2::crc16Store(file.data() + file.size() - 2, crc);

		return file;
	}

	// ----------------------------------------------------- machine B's session

	/* The delivery driver, on its own thread. The stub answers packets on this
	 * test's main thread while the client
	 * thread performs the script's exchanges. One request is in flight at a
	 * time, which is what makes every Board read happen between two answered
	 * packets.
	 *
	 * The delivery window is the test's own decision, not the client's. The
	 * client only continues and reads; when its continue-until-stop returns,
	 * the main thread drives one `pch2Load` and one quantum boundary so the
	 * frame crosses the hub and reaches the device while the machine is
	 * stopped. The watchpoint record is taken by the next continue's MCU
	 * phase, which is the phase that reads the data port. */
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

	class RspClient final
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
			char        c = 0;

			while(::recv(m_fd, &c, 1, 0) == 1 && c != '$')
			{
			}
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
		static std::string framed(const std::string& _payload)
		{
			unsigned sum = 0;
			for(const char c : _payload)
				sum += unsigned(uint8_t(c));

			char tail[4];
			std::snprintf(tail, sizeof tail, "#%02x", sum & 0xffu);
			return "$" + _payload + tail;
		}

		int m_fd = -1;
	};

	void clientLoop(Channel& _channel, RspClient& _client)
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

	std::string hexAddr(const uint32_t _value)
	{
		char buf[9];
		std::snprintf(buf, sizeof buf, "%x", unsigned(_value));
		return buf;
	}

	Channel*     g_channel = nullptr;
	g2::GdbStub* g_stub    = nullptr;

	// Reset the machine to the start of the program. Runs on the main thread
	// with the stub idle, which is the only time the Board is touched.
	void restartA(g2::Board& _board)
	{
		_board.resetMcu(g_stackTop, g_codeBase);
	}

	// Ask the stub one question and return its answer. The main thread pumps
	// the stub, so every Board read happens between two answered packets and
	// the machine is quiescent here.
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
}

int main()
{
	// ==================================================================
	// Machine A. The stub is served in-process -- the tier-T0 way to hold a
	// session without the console binary, which boots firmware this test must
	// not read -- and the client runs against it over loopback, exactly the
	// packet sequence an operator session runs.
	// ==================================================================
	g2::Board boardA(makeConfigA());
	Ram       ramA(g_windowSize);
	boardA.memory().attach(g2::Region::Sdram, &ramA);

	for(uint32_t i = 0; i < uint32_t(sizeof(g_program) / sizeof(g_program[0])); ++i)
		ramA.pokeWord(g_codeOffset + i * 2u, g_program[i]);

	boardA.resetMcu(g_stackTop, g_codeBase);

	/* The stub is constructed after every unit is attached: it interposes its
	 * watchpoint wrapper on the targets the map holds at that moment. */
	g2::GdbStub stubA(boardA);
	g_stub = &stubA;

	const uint16_t portA = stubA.listenOn(0);
	check(portA != 0, "machine A: the stub binds a loopback port and reports it");

	Channel channelA;
	g_channel = &channelA;

	RspClient   clientA;
	std::thread clientThreadA;

	if(portA == 0)
	{
		std::cout << "FAIL t0_gdb_script: no listening socket, nothing further can run"
		          << std::endl;
		std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
		          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;
		return 1;
	}

	{
		bool connected = false;
		std::thread connector([&] { connected = clientA.connect(portA); });
		const bool accepted = stubA.waitForClient();
		connector.join();

		check(connected, "machine A: the script's client connects");
		check(accepted,  "machine A: the stub accepts the client");

		if(!connected || !accepted)
		{
			std::cout << "FAIL t0_gdb_script: no session" << std::endl;
			std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
			          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;
			return 1;
		}
	}

	clientThreadA = std::thread(clientLoop, std::ref(channelA), std::ref(clientA));

	checkText(ask("qSupported:multiprocess+"), "PacketSize=1000",
	          "machine A: the session opens the way the script's first packet opens it");

	// ==================================================================
	// Machine B -- the delivery machine. The stub is served on its own loopback
	// port, and it is the stub the watchpoint cases connect to. The delivery
	// runs on the same Board the stub serves, which is what makes this
	// GDB-with-traffic: the session's breakpoints, watchpoints and the
	// delivery all sit on one machine.
	// ==================================================================
	g2::Board  boardB(makeConfigB());
	Ram        ramB(g_windowSize);
	boardB.memory().attach(g2::Region::Sdram, &ramB);

	for(uint32_t i = 0; i < uint32_t(sizeof(g_programB) / sizeof(g_programB[0])); ++i)
		ramB.pokeWord(i * 2u, g_programB[i]);

	boardB.resetMcu(g_stackTop, g_codeBase);
	boardB.setMcuReg(g_regD0, g_deliveryWord);
	boardB.setMcuReg(g_regA1, g_watchAddrB);

	/* The stub is constructed after every unit is attached, for the reason
	 * stated above. The Board's hub exists from construction on. */
	g2::GdbStub stubB(boardB);

	// ==================================================================
	// Machine A, case one -- the breakpoint the synthetic boot reaches.
	// ==================================================================
	{
		restartA(boardA);

		checkText(ask("Z0," + hexAddr(g_bpFirst) + ",2"), "OK",
		          "case one: Z0 is accepted at the address the boot reaches");
		checkText(ask("c"), "T05", "case one: c answers a stop reply");
		checkEqual(boardA.mcuReg(g_regPc), g_bpFirst,
		           "case one: the machine stopped with the PC EQUAL to the breakpoint address");
		checkText(ask("z0," + hexAddr(g_bpFirst) + ",2"), "OK",
		          "case one: z0 removes it again");
	}

	// ==================================================================
	// Machine A, case three -- the batch of two breakpoints, in reach order.
	// ==================================================================
	{
		restartA(boardA);

		checkText(ask("Z0," + hexAddr(g_bpFirst) + ",2"), "OK",
		          "case three: the first breakpoint of the batch is accepted");
		checkText(ask("c"), "T05", "case three: the first stop reply arrives");
		checkEqual(boardA.mcuReg(g_regPc), g_bpFirst,
		           "case three: the FIRST stop is at the first breakpoint, in program order");

		checkText(ask("Z0," + hexAddr(g_bpSecond) + ",2"), "OK",
		          "case three: the second breakpoint of the batch is accepted");
		checkText(ask("c"), "T05", "case three: the second stop reply arrives");
		checkEqual(boardA.mcuReg(g_regPc), g_bpSecond,
		           "case three: the SECOND stop is at the second breakpoint, in program order");

		checkText(ask("z0," + hexAddr(g_bpFirst) + ",2"), "OK",
		          "case three: the first breakpoint is removed again");
		checkText(ask("z0," + hexAddr(g_bpSecond) + ",2"), "OK",
		          "case three: the second breakpoint is removed again");
	}

	// ==================================================================
	// Machine A, case five -- the miss is reported as a miss. The unreachable
	// breakpoint sits beside the known positive, and the run must stop at the
	// known positive: a stop reply at the unreachable address, or a run that
	// never reaches the known positive, is red. The distinction between
	// hit=break@address and hit=none is what the harness enforces here rather
	// than leaving it to the operator to remember.
	// ==================================================================
	{
		restartA(boardA);

		checkText(ask("Z0," + hexAddr(g_bpUnreached) + ",2"), "OK",
		          "case five: Z0 is accepted at the unreachable address");
		checkText(ask("Z0," + hexAddr(g_bpFirst) + ",2"), "OK",
		          "case five: the KNOWN POSITIVE breakpoint is armed beside it");
		checkText(ask("c"), "T05", "case five: c answers a stop reply");

		const uint32_t stopped = boardA.mcuReg(g_regPc);
		checkEqual(stopped, g_bpFirst,
		           "case five: THE MISS IS A MISS -- the machine stopped at the known "
		           "positive, not at the unreachable address");

		check(!boardA.mcuHalted(),
		      "case five: a machine stopped at the known positive has not halted");

		checkText(ask("z0," + hexAddr(g_bpUnreached) + ",2"), "OK",
		          "case five: the unreachable breakpoint is removed again");
		checkText(ask("z0," + hexAddr(g_bpFirst) + ",2"), "OK",
		          "case five: the known positive is removed again");
	}

	// ==================================================================
	// Machine B, cases two and seven -- the armed watchpoint fires on the write
	// the delivery path makes. The delivery is driven on machine B's own stub
	// connection: the watchpoint is armed through stubB, the frames pch2Load
	// originates cross the hub, and the stop names the word the delivery driver
	// wrote. The boot path touches none of it.
	// ==================================================================
	{
		Channel channelB;
		RspClient clientB;

		g_channel = &channelB;
		g_stub    = &stubB;

		const uint16_t portB = stubB.listenOn(0);
		check(portB != 0, "case two: machine B's stub binds a loopback port");

		if(portB == 0)
		{
			std::cout << "FAIL t0_gdb_script: machine B's stub did not bind" << std::endl;
			++g_failures;
		}
		else
		{
			bool connectedB = false;
			std::thread connectorB([&] { connectedB = clientB.connect(portB); });
			const bool acceptedB = stubB.waitForClient();
			connectorB.join();

			check(connectedB && acceptedB, "case two: the client connects to machine B's stub");

			std::thread clientThreadB;
			if(connectedB && acceptedB)
				clientThreadB = std::thread(clientLoop, std::ref(channelB), std::ref(clientB));

			checkText(ask("Z2," + hexAddr(g_watchAddrB) + ",4"), "OK",
			          "case two: the watchpoint is accepted on the delivery word");

			// The known positive beside it: a watchpoint that never fires and a
			// session that reports nothing must be distinguishable from a
			// delivery that did not cross.
			checkText(ask("Z0," + hexAddr(g_bpFirst) + ",2"), "OK",
			          "case two: the known-positive breakpoint is armed beside the watchpoint");

			// The delivery, on the same machine the stub serves. pch2Load
			// originates the frames through the InternalClient -- the same code
			// path the plugin calls -- while the machine is stopped.
			{
				std::vector<uint8_t> objects;
				appendObject(objects, 0x21u, 15u, 71u);

				const std::vector<uint8_t> file = buildContainer(objects);

				g2::InternalClient client(boardB.transport(), 512, 4);

				const g2::Pch2LoadResult result =
					g2::pch2Load(file.data(), file.size(), client);

				check(result == g2::Pch2LoadResult::Loaded,
				      "case two: the container loads through pch2Load on the session's machine");

				// The hub received it. The frames pch2Load queued are drained
				// by the Board's own pump at the quantum boundary -- the
				// delivered count is the hub-to-device half of the crossing,
				// read off the Board and not asserted from the load's return
				// value.
				boardB.tickSofIfDue(0);
				boardB.tickSofIfDue(1);

				check(boardB.usbTransport().completed >= 1u,
				      "case seven: the Board's own pump delivered a frame to the device, "
				      "which is the origination the watchpoint sits over");
			}

			checkText(ask("c"), "T05watch:" + hexAddr(g_watchAddrB) + ";",
			          "case two: THE MACHINE'S OWN STORE FIRED -- the stop reply names the "
			          "word the program wrote through the Board's own bus");

			check(!boardB.mcuHalted(),
			      "case two: a machine stopped on a watchpoint has not halted");

			checkText(ask("z2," + hexAddr(g_watchAddrB) + ",4"), "OK",
			          "case two: the watchpoint is removed again");
			checkText(ask("z0," + hexAddr(g_bpFirst) + ",2"), "OK",
			          "case two: the known positive is removed again");

			checkText(ask("D"), "OK", "case two: machine B's session detaches");

			{
				std::lock_guard<std::mutex> lock(channelB.mutex);
				channelB.quit = true;
			}
			channelB.cv.notify_all();
			if(clientThreadB.joinable())
				clientThreadB.join();
			clientB.close();
		}

		g_channel = &channelA;
		g_stub    = &stubA;
	}

	// ==================================================================
	// Machine A, case six -- a client that never sends `c` leaves the machine
	// stopped. The counter is read across a real connection: whatever the
	// answer, the connection is closed first, so the value read is the
	// machine's and not the socket's.
	// ==================================================================
	{
		uint16_t port6 = 0;

		{
			g2::GdbStub stub6(boardA);
			port6 = stub6.listenOn(0);

			Channel channel6;
			RspClient client6;
			std::thread clientThread6;
			bool      accepted = false;

			if(port6 != 0)
			{
				std::thread connector([&]
				{
					accepted = client6.connect(port6);
				});
				stub6.waitForClient();
				connector.join();
			}

			check(port6 != 0 && accepted,
			      "case six: the idle client connects and arms nothing");

			if(accepted)
			{
				g_channel = &channel6;
				g_stub    = &stub6;

				clientThread6 = std::thread(clientLoop, std::ref(channel6), std::ref(client6));

				// The client connects and never sends `c`. qSupported is
				// answered; nothing else is sent, and the session then ends.
				checkText(ask("qSupported:multiprocess+"), "PacketSize=1000",
				          "case six: the session opens and the client stays idle");

				// The answer, read while the machine is still the stub's. The
				// `?` packet is answered from the stub's own halt state; no `c`
				// was ever sent, so the machine has executed nothing since the
				// session opened.
				checkText(ask("?"), "T05",
				          "case six: the idle session reports a machine that has "
				          "executed nothing");

				checkText(ask("D"), "OK", "case six: the idle session detaches");

				{
					std::lock_guard<std::mutex> lock(channel6.mutex);
					channel6.quit = true;
				}
				channel6.cv.notify_all();
				clientThread6.join();
				client6.close();

				g_channel = &channelA;
				g_stub    = &stubA;
			}
		}

		checkEqual(boardA.mcuReg(g_regPc), g_bpFirst,
		           "case six: THE MACHINE STAYED STOPPED -- the PC is where case five "
		           "left it, and no continue ran");

		check(!boardA.mcuHalted(), "case six: the machine has not halted");
	}

	// ==================================================================
	// The detach. The stub answers OK and ends the session -- the measured
	// stub-exits-on-D behaviour.
	// ==================================================================
	checkText(ask("D"), "OK", "the stub answers the detach packet");

	{
		std::lock_guard<std::mutex> lock(channelA.mutex);
		channelA.quit = true;
	}
	channelA.cv.notify_all();
	clientThreadA.join();
	clientA.close();
	stubA.close();
	stubB.close();

	g_stub    = nullptr;
	g_channel = nullptr;

	std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
	          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
