// The GDB remote stub on the MCF5307. Tier T0: this test needs no firmware
// artifact of any kind.
//
// It drives the stub with a test client over a loopback socket and no `gdb`
// binary, so the check runs anywhere the suite does. It asserts the answers and
// not the acceptance: a stub that replies `OK` to everything fails every case
// below.
//
// The client runs on its own thread and the stub runs on this one, which is the
// arrangement that makes the Board race-free. The client thread owns the socket
// and nothing else; every read of the Board and every seed of a register happens
// on the main thread, between one answered packet and the next. `ask()` below is
// the whole protocol: it hands the request to the client thread, pumps the stub
// until the reply has come back, and returns it.
//
// The program is the same shape t0_board_mcu_handle.cpp uses, and its provenance
// is the same: the opcodes are hand-encoded from the MOVE format and the two
// halting words are the ones that file already pins. Every address below is this
// test's own configuration, because no authority records a size for the SDRAM
// window.
//
// The negative breakpoint case is what makes the positive one mean something,
// and it is run in two forms because two different defects reach it. Form A sets
// a breakpoint at an address that is never a program counter -- an odd address
// inside the code -- so a stub comparing with `>=` stops the machine where a
// stub comparing with `==` runs it to the halt. Form B sets a breakpoint at an
// address the machine does reach and then removes it, so a stub that forgets to
// remove stops where a correct stub does not.

#include "board.h"
#include "gdbStub.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	std::string hex32(const uint32_t _value)
	{
		static const char* digits = "0123456789abcdef";
		std::string result = "0x";
		for(int shift = 28; shift >= 0; shift -= 4)
			result += digits[(_value >> shift) & 0xfu];
		return result;
	}

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
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

	// ---------------------------------------------------------------- the memory

	/* Byte-addressed memory, big-endian, which is what the 68000 family is.
	 * Copied in shape from t0_board_mcu_handle.cpp: the assertions here are about
	 * what the stub answers, not about what arrived at the unit. */
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

	// ---------------------------------------------------------------- the layout

	constexpr uint32_t g_windowBase = g2::g_sdramBase;
	constexpr uint32_t g_windowSize = 0x1000u;
	constexpr uint32_t g_stackTop   = g_windowBase + 0x800u;

	constexpr uint32_t g_codeOffset  = 0x000u;
	constexpr uint32_t g_watchOffset = 0x200u;
	constexpr uint32_t g_scratchOffset = 0x300u;

	constexpr uint32_t g_codeBase    = g_windowBase + g_codeOffset;
	constexpr uint32_t g_watchAddr   = g_windowBase + g_watchOffset;
	constexpr uint32_t g_scratchAddr = g_windowBase + g_scratchOffset;

	/* The program, hand-encoded.
	 *
	 *     +0x00  4E71  nop
	 *     +0x02  4E71  nop
	 *     +0x04  4E71  nop
	 *     +0x06  2280  move.l d0,(a1)   the watched write
	 *     +0x08  4E71  nop              the reached breakpoint address
	 *     +0x0A  A001  line-A, which faults and halts
	 *
	 * A nop is the step target and that is deliberate. It costs more than one
	 * cycle -- the fetch alone is priced at two in the core's own cpu.nim -- so
	 * "one instruction" and "one cycle" are different amounts of progress, which
	 * is exactly what the step case has to tell apart. */
	constexpr uint16_t g_program[] = {0x4E71u, 0x4E71u, 0x4E71u, 0x2280u, 0x4E71u, 0xA001u};

	constexpr uint32_t g_insBytes = 2u;

	// The breakpoint the machine does reach: the nop after the store.
	constexpr uint32_t g_bpReached = g_codeBase + 0x008u;

	/* The breakpoint the machine does not reach. It is odd and inside the code,
	 * so no program counter can ever equal it, and every program counter after
	 * the first step is greater than it. A stub comparing with `>=` stops here;
	 * a stub comparing with `==` never does. */
	constexpr uint32_t g_bpUnreached = g_codeBase + 0x001u;

	// The value the store moves, distinct in every byte so a transfer of the
	// wrong width lands on a value no other width produces.
	constexpr uint32_t g_operand = 0xC0DEDA7Au;

	constexpr int g_regD0 = 0;
	constexpr int g_regA1 = 9;
	constexpr int g_regSr = 16;
	constexpr int g_regPc = 17;

	constexpr int g_regCount = 18;

	g2::BoardConfig makeConfig()
	{
		g2::BoardConfig config;
		config.memory.sdram = {g_windowBase, g_windowSize};
		return config;
	}

	// ---------------------------------------------------------------- the client

	std::string hexByte(const uint8_t _value)
	{
		char buf[3];
		std::snprintf(buf, sizeof buf, "%02x", unsigned(_value));
		return buf;
	}

	std::string hexWord32(const uint32_t _value)
	{
		char buf[9];
		std::snprintf(buf, sizeof buf, "%08x", unsigned(_value));
		return buf;
	}

	// The address form a stop reply and a Z packet carry: hexadecimal, no 0x, no
	// leading zeroes, which is what GDB itself writes.
	std::string hexAddr(const uint32_t _value)
	{
		char buf[9];
		std::snprintf(buf, sizeof buf, "%x", unsigned(_value));
		return buf;
	}

	std::string framed(const std::string& _payload)
	{
		unsigned sum = 0;
		for(const char c : _payload)
			sum += unsigned(uint8_t(c));

		char tail[4];
		std::snprintf(tail, sizeof tail, "#%02x", sum & 0xffu);
		return "$" + _payload + tail;
	}

	/* The test client. It lives on its own thread, owns the socket and touches
	 * nothing else -- no Board, no stub. The main thread hands it one request at
	 * a time and collects one reply, so the two threads share exactly the queue
	 * below and the kernel's socket. */
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

		// Send one request, read the acknowledgement and the reply packet, and
		// return the reply's payload. Runs on the client thread only.
		std::string exchange(const std::string& _payload)
		{
			const std::string out = framed(_payload);

			if(::send(m_fd, out.data(), out.size(), 0) != ssize_t(out.size()))
				return "<send failed>";

			// The acknowledgement, then the packet. A '-' means the stub read a
			// bad checksum, which is a failure this test reports rather than
			// retries.
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
		int m_fd = -1;
	};

	/* The two-thread rendezvous. The main thread posts a request and then pumps
	 * the stub; the client thread performs the exchange and posts the reply. One
	 * request is in flight at a time, which is what makes every Board read on the
	 * main thread happen between two answered packets. */
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
				request               = _channel.request;
				_channel.hasRequest   = false;
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

	Channel*      g_channel = nullptr;
	g2::GdbStub*  g_stub    = nullptr;

	/* Ask the stub one question and return its answer. The pump is what runs the
	 * stub: the main thread is inside servePacket() for exactly as long as the
	 * stub is answering, and back here -- with the machine quiescent -- when it
	 * has answered. */
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

	// Reset the machine and seed the two registers the program needs. Runs on the
	// main thread with the stub idle, which is the only time the Board is touched
	// from here.
	void restart(g2::Board& _board)
	{
		_board.resetMcu(g_stackTop, g_codeBase);
		_board.setMcuReg(g_regD0, g_operand);
		_board.setMcuReg(g_regA1, g_watchAddr);
	}

	// What Board::onRead answers for one byte, which is the value the `m` packet
	// has to reproduce.
	uint8_t boardByte(g2::Board& _board, const uint32_t _address)
	{
		mcf5307_bus_status status = MCF5307_BUS_OK;
		return uint8_t(g2::Board::onRead(&_board, _address, 1, &status) & 0xffu);
	}
}

int main()
{
	g2::Board board(makeConfig());

	Ram ram(g_windowSize);
	board.memory().attach(g2::Region::Sdram, &ram);

	for(uint32_t i = 0; i < uint32_t(sizeof(g_program) / sizeof(g_program[0])); ++i)
		ram.pokeWord(g_codeOffset + i * 2u, g_program[i]);

	restart(board);

	/* The stub is constructed after every unit is attached, because it interposes
	 * its watchpoint wrapper on the targets the map holds at that moment. */
	g2::GdbStub stub(board);
	g_stub = &stub;

	const uint16_t port = stub.listenOn(0);
	check(port != 0, "the stub binds a loopback port and reports it");
	if(port == 0)
	{
		std::cout << "FAIL t0_gdb_stub: no listening socket, nothing further can run" << std::endl;
		return 1;
	}

	Channel channel;
	g_channel = &channel;

	Client      client;
	std::thread clientThread;

	{
		bool connected = false;
		std::thread connector([&] { connected = client.connect(port); });

		const bool accepted = stub.waitForClient();
		connector.join();

		check(connected, "the test client connects to the stub's loopback port");
		check(accepted,  "the stub accepts the client");

		if(!connected || !accepted)
		{
			std::cout << "FAIL t0_gdb_stub: no session, nothing further can run" << std::endl;
			return 1;
		}
	}

	clientThread = std::thread(clientLoop, std::ref(channel), std::ref(client));

	// ==================================================================
	// PHASE ONE -- the session opens.
	// ==================================================================
	{
		checkText(ask("qSupported:multiprocess+"), "PacketSize=1000",
		          "phase one: qSupported answers the one capability the stub has");
		checkText(ask("?"), "T05",
		          "phase one: ? reports the halt reason of a machine waiting to run");
		checkText(ask("qC"), "",
		          "phase one: an unsupported packet is answered with the empty reply");
	}

	// ==================================================================
	// PHASE TWO -- `s` advances the program counter by one instruction.
	// ==================================================================
	{
		const uint32_t before = board.mcuReg(g_regPc);
		checkEqual(before, g_codeBase, "phase two: the machine starts at the code base");

		checkText(ask("s"), "T05", "phase two: s answers a stop reply");
		checkEqual(board.mcuReg(g_regPc), g_codeBase + g_insBytes,
		           "phase two: one s advances the PC by one instruction and not by one cycle");

		ask("s");
		ask("s");
		checkEqual(board.mcuReg(g_regPc), g_codeBase + 3u * g_insBytes,
		           "phase two: three s packets advance the PC by three instructions");
	}

	// ==================================================================
	// PHASE THREE -- `g` answers from the core and not from a copy.
	// ==================================================================
	{
		std::string expected;
		for(int i = 0; i < g_regCount; ++i)
			expected += hexWord32(board.mcuReg(i));

		const std::string actual = ask("g");

		checkEqual(uint32_t(actual.size()), uint32_t(g_regCount * 8),
		           "phase three: g returns eighteen 32-bit registers");
		checkText(actual, expected,
		          "phase three: every register g returns is the one Board::mcuReg answers");

		const std::string pcField = actual.size() >= size_t(g_regCount * 8)
			? actual.substr(size_t(g_regPc) * 8u, 8u)
			: std::string();
		checkText(pcField, hexWord32(board.mcuReg(g_regPc)),
		          "phase three: the PC g returns equals Board::mcuReg(17)");

		/* The second `g` is the one that refuses a cache. The machine has moved
		 * between the two, so a stub answering from a copy taken at the first `g`
		 * -- or at the connect -- returns the older program counter here. */
		ask("s");

		std::string moved;
		for(int i = 0; i < g_regCount; ++i)
			moved += hexWord32(board.mcuReg(i));

		checkText(ask("g"), moved,
		          "phase three: a g after a step reports the moved PC and not a cached one");
	}

	// ==================================================================
	// PHASE FOUR -- `G` writes the register file.
	// ==================================================================
	{
		restart(board);

		std::vector<uint32_t> wanted(g_regCount, 0u);
		for(int i = 0; i < g_regCount; ++i)
			wanted[size_t(i)] = 0x11110000u + uint32_t(i);

		// The status register keeps its low sixteen bits only: mcf5307.h says so
		// and machine.nim masks it, so the expected value is masked here rather
		// than the assertion being loosened.
		wanted[size_t(g_regSr)] = 0x00002700u;

		// The program counter is read-only through this call. mcf5307.h states
		// it at index 17 and machine.nim's regFileSet has no branch for it, so
		// the stub sends the value and the core keeps its own.
		const uint32_t pcBefore = board.mcuReg(g_regPc);

		std::string payload = "G";
		for(int i = 0; i < g_regCount; ++i)
			payload += hexWord32(wanted[size_t(i)]);

		checkText(ask(payload), "OK", "phase four: G is accepted");

		bool allWritten = true;
		for(int i = 0; i < g_regCount; ++i)
		{
			if(i == g_regPc)
				continue;
			if(board.mcuReg(i) != wanted[size_t(i)])
				allWritten = false;
		}

		check(allWritten, "phase four: G writes every writable register of the file");
		checkEqual(board.mcuReg(g_regPc), pcBefore,
		           "phase four: G leaves the read-only program counter alone");
	}

	// ==================================================================
	// PHASE FIVE -- `m` and `M` take the Board's own bus path.
	// ==================================================================
	{
		restart(board);

		std::string expected;
		for(uint32_t i = 0; i < 8u; ++i)
			expected += hexByte(boardByte(board, g_codeBase + i));

		checkText(ask("m" + hexAddr(g_codeBase) + ",8"), expected,
		          "phase five: m returns the bytes Board::onRead returns for the same addresses");

		checkText(ask("M" + hexAddr(g_scratchAddr) + ",4:deadbeef"), "OK",
		          "phase five: M is accepted");

		std::string readBack;
		for(uint32_t i = 0; i < 4u; ++i)
			readBack += hexByte(boardByte(board, g_scratchAddr + i));

		checkText(readBack, "deadbeef",
		          "phase five: the bytes M wrote read back through Board::onRead");
	}

	// ==================================================================
	// PHASE SIX -- the breakpoint the machine reaches.
	// ==================================================================
	{
		restart(board);

		checkText(ask("Z0," + hexAddr(g_bpReached) + ",2"), "OK",
		          "phase six: Z0 is accepted at an address the machine reaches");
		checkText(ask("c"), "T05", "phase six: c stops and reports a stop reply");
		checkEqual(board.mcuReg(g_regPc), g_bpReached,
		           "phase six: the machine stopped with the PC EQUAL to the breakpoint address");
		check(!board.mcuHalted(),
		      "phase six: a machine stopped at a breakpoint has not halted");

		checkText(ask("z0," + hexAddr(g_bpReached) + ",2"), "OK",
		          "phase six: z0 is accepted");
	}

	// ==================================================================
	// PHASE SEVEN -- the breakpoint the machine does not reach.
	//
	// This is what makes phase six mean something. An address no program counter
	// can equal must not stop the machine, and the machine must reach its own
	// halt instead.
	// ==================================================================
	{
		restart(board);

		checkText(ask("Z0," + hexAddr(g_bpUnreached) + ",2"), "OK",
		          "phase seven: Z0 is accepted at an address the machine never reaches");
		checkText(ask("c"), "T05", "phase seven: c answers a stop reply");

		check(board.mcuReg(g_regPc) != g_bpUnreached,
		      "phase seven: the machine did not stop at the unreachable breakpoint address");
		check(board.mcuHalted(),
		      "phase seven: the machine ran on to its own halt instead of stopping");
		check(board.faulted(),
		      "phase seven: the halt is the refused word's fault and not a breakpoint");

		checkText(ask("z0," + hexAddr(g_bpUnreached) + ",2"), "OK",
		          "phase seven: z0 is accepted");
	}

	// ==================================================================
	// PHASE EIGHT -- a removed breakpoint is really removed.
	// ==================================================================
	{
		restart(board);

		checkText(ask("Z0," + hexAddr(g_bpReached) + ",2"), "OK",
		          "phase eight: Z0 is accepted");
		checkText(ask("z0," + hexAddr(g_bpReached) + ",2"), "OK",
		          "phase eight: z0 removes it again");
		checkText(ask("c"), "T05", "phase eight: c answers a stop reply");

		check(board.mcuReg(g_regPc) != g_bpReached,
		      "phase eight: the machine did not stop at the REMOVED breakpoint address");
		check(board.mcuHalted(),
		      "phase eight: the machine ran on to its own halt instead of stopping");
	}

	// ==================================================================
	// PHASE NINE -- the write watchpoint reports the write and names it.
	// ==================================================================
	{
		restart(board);

		checkText(ask("Z2," + hexAddr(g_watchAddr) + ",4"), "OK",
		          "phase nine: Z2 is accepted on the address the program stores to");
		checkText(ask("c"), "T05watch:" + hexAddr(g_watchAddr) + ";",
		          "phase nine: the stop reply names the watched address that was written");
		check(!board.mcuHalted(),
		      "phase nine: a machine stopped on a watchpoint has not halted");

		std::string stored;
		for(uint32_t i = 0; i < 4u; ++i)
			stored += hexByte(boardByte(board, g_watchAddr + i));
		checkText(stored, "c0deda7a",
		          "phase nine: the write the watchpoint reported is the one the program made");

		checkText(ask("z2," + hexAddr(g_watchAddr) + ",4"), "OK",
		          "phase nine: z2 is accepted");
	}

	// ==================================================================
	// PHASE TEN -- a removed watchpoint stops nothing.
	// ==================================================================
	{
		restart(board);

		checkText(ask("c"), "T05", "phase ten: c answers a stop reply");
		check(board.mcuHalted(),
		      "phase ten: with the watchpoint removed the machine runs on to its own halt");
	}

	// ==================================================================
	// PHASE ELEVEN -- the read watchpoint.
	// ==================================================================
	{
		restart(board);

		// The instruction fetch at the code base is a read the core makes on its
		// own, so a read watchpoint there fires on the very first step.
		checkText(ask("Z3," + hexAddr(g_codeBase) + ",2"), "OK",
		          "phase eleven: Z3 is accepted");
		checkText(ask("c"), "T05rwatch:" + hexAddr(g_codeBase) + ";",
		          "phase eleven: the stop reply names the watched address that was read");
		checkText(ask("z3," + hexAddr(g_codeBase) + ",2"), "OK",
		          "phase eleven: z3 is accepted");
	}

	// ------------------------------------------------------------------
	// The session ends. `D` detaches, which is what a debugger sends when it
	// lets the machine go.
	{
		checkText(ask("D"), "OK", "the stub answers the detach packet");
	}

	{
		std::lock_guard<std::mutex> lock(channel.mutex);
		channel.quit = true;
	}
	channel.cv.notify_all();
	clientThread.join();
	client.close();
	stub.close();

	std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
	          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;

	return g_failures == 0 ? 0 : 1;
}
