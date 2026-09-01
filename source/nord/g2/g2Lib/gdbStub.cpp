// A packet is `$<payload>#<two hex checksum digits>`, the checksum is the sum
// of the payload bytes modulo 256, and each side answers `+` for a packet it
// read and `-` for one whose checksum did not match.

#include "gdbStub.h"

/* The socket layer is the platform-dependent part of this file, confined to
 * the block below and to the wrappers under it. Winsock is Berkeley sockets
 * with these differences and no more: the handle is a UINT_PTR rather than an
 * int, the library needs starting once per process, close is spelled
 * closesocket, and the byte pointers of setsockopt, send and recv are char*
 * rather than void*. Each difference is answered once here so that the
 * protocol code below reads the same on every platform. */
#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <winsock2.h>
#	include <ws2tcpip.h>
#else
#	include <arpa/inet.h>
#	include <netinet/in.h>
#	include <netinet/tcp.h>
#	include <sys/socket.h>
#	include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace g2
{
	namespace
	{
		// The registers GDB's m68k target expects, in its order, which is the
		// register file's own order. mcf5307.h owns the mapping.
		constexpr int g_regCount = 18;
		constexpr int g_regPc    = 17;

		/* The continue bound is a stop and not a figure any authority publishes.
		 * It exists so that a machine which never reaches a breakpoint, a
		 * watchpoint or its own halt terminates rather than hanging the debugger.
		 * A `c` that leaves through this bound answers the same stop reply as one
		 * that halted, which is what a debugger can act on. */
		constexpr uint64_t g_continueBound = 100000000ull;

		/* The same stop, counted in quanta, for a session that drives the whole
		 * machine. Its size is chosen so that it is not reached by anything the
		 * machine legitimately does: `g2TestConsole --boot` drives 500000 quanta
		 * to reach a settled patch browser, and this is twice that. A `c` that
		 * leaves through it answers the same stop reply as one that halted. */
		constexpr uint64_t g_continueQuantumBound = 1000000ull;

		/* The quanta one `s` may turn. A step needs one quantum in which the MCU
		 * actually runs; it needs more than one only when the quanta before it
		 * take `g2::runQuantum`'s long-dispatch branch, which runs no MCU cycles
		 * and pays the carried debt down by one whole allocation each time. That
		 * branch therefore cannot repeat indefinitely, and this bound exists so
		 * that a step terminates even if it did. */
		constexpr uint64_t g_stepQuantumBound = 1024ull;

		// The allowance a `c` carries: every instruction the bound permits.
		constexpr uint64_t g_unbounded = ~uint64_t(0);

		// The size a byte access presents to Board::onRead and Board::onWrite, in
		// the core's unit: mcf5307.h states it once per callback typedef, and
		// `size` there is a count of BYTES and never a width in bits.
		constexpr int g_byte = 1;

		// The signal a stop reply carries. SIGTRAP is what a debugger expects
		// from a breakpoint, a watchpoint and a completed step alike.
		const char* const g_sigTrap = "05";

		/* The socket wrappers. Each takes and returns the intptr_t handle
		 * GdbStub stores, so nothing below this point names a platform
		 * socket type. */

#ifdef _WIN32
		using PlatformSocket = SOCKET;
#else
		using PlatformSocket = int;
#endif

		// The stored handle as the platform's own socket type. The calls that
		// take no byte pointer -- bind, listen, accept, getsockname -- differ
		// in nothing else and are spelled directly with this.
		PlatformSocket socketArg(const std::intptr_t _fd)
		{
			return static_cast<PlatformSocket>(_fd);
		}

		/* Starts Winsock once per process and answers whether sockets can be
		 * created at all. Winsock refuses every call before WSAStartup and
		 * there is no other place a library-only component can do it: the
		 * stub has no process entry point of its own. On POSIX there is
		 * nothing to start and the answer is unconditionally yes. */
		bool socketsReady()
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

		std::intptr_t socketClose(const std::intptr_t _fd)
		{
#ifdef _WIN32
			return ::closesocket(socketArg(_fd));
#else
			return ::close(socketArg(_fd));
#endif
		}

		/* setsockopt's value pointer is const char* on Winsock and const
		 * void* on POSIX, and its length is int on both. A const char* is
		 * the one spelling both accept. */
		int socketSetOption(const std::intptr_t _fd, const int _level, const int _option,
			const int _value)
		{
			const int value = _value;
#ifdef _WIN32
			return ::setsockopt(socketArg(_fd), _level, _option,
				reinterpret_cast<const char*>(&value), int(sizeof value));
#else
			return ::setsockopt(socketArg(_fd), _level, _option,
				reinterpret_cast<const char*>(&value), socklen_t(sizeof value));
#endif
		}

		/* Answers the bytes sent, or a value <= 0 for a closed or failed
		 * connection. Winsock's length and return are int; POSIX's are
		 * size_t and ssize_t. The narrowing to int is safe because every
		 * caller here sends a packet, and PacketSize is 0x1000. */
		long socketSend(const std::intptr_t _fd, const char* _data, const size_t _size)
		{
#ifdef _WIN32
			return long(::send(socketArg(_fd), _data, int(_size), 0));
#else
			return long(::send(socketArg(_fd), _data, _size, 0));
#endif
		}

		// Answers the bytes read. Every caller here reads exactly one byte.
		long socketRecv(const std::intptr_t _fd, char* _data, const size_t _size)
		{
#ifdef _WIN32
			return long(::recv(socketArg(_fd), _data, int(_size), 0));
#else
			return long(::recv(socketArg(_fd), _data, _size, 0));
#endif
		}

		bool hexDigit(const char _c, unsigned& _value)
		{
			if(_c >= '0' && _c <= '9') { _value = unsigned(_c - '0');        return true; }
			if(_c >= 'a' && _c <= 'f') { _value = unsigned(_c - 'a') + 10u;  return true; }
			if(_c >= 'A' && _c <= 'F') { _value = unsigned(_c - 'A') + 10u;  return true; }
			return false;
		}

		/* Read a hexadecimal number out of `_text` starting at `_pos`, leaving
		 * `_pos` on the first character that is not a hexadecimal digit. It
		 * answers false when there was no digit at all, so an empty field is a
		 * malformed packet rather than a silent zero. */
		bool parseHex(const std::string& _text, size_t& _pos, uint32_t& _value)
		{
			unsigned digit = 0;
			size_t   start = _pos;
			uint32_t value = 0;

			while(_pos < _text.size() && hexDigit(_text[_pos], digit))
			{
				value = (value << 4) | digit;
				++_pos;
			}

			if(_pos == start)
				return false;

			_value = value;
			return true;
		}

		std::string hexAddress(const uint32_t _value)
		{
			char buf[9];
			std::snprintf(buf, sizeof buf, "%x", unsigned(_value));
			return buf;
		}

		std::string hexWord32(const uint32_t _value)
		{
			char buf[9];
			std::snprintf(buf, sizeof buf, "%08x", unsigned(_value));
			return buf;
		}

		std::string hexByte(const uint8_t _value)
		{
			char buf[3];
			std::snprintf(buf, sizeof buf, "%02x", unsigned(_value));
			return buf;
		}

		// The error reply. GDB reads `E NN` as "the command failed"; the number
		// is the stub's own and no debugger interprets it.
		const char* const g_error = "E01";

		// Every region the memory map decodes. Region::None is not one of them:
		// it is the answer for an address no window claims.
		constexpr Region g_regions[] = {
			Region::Cs0, Region::Cs1, Region::Cs2, Region::Cs3,
			Region::Cs4, Region::Cs5, Region::Mbar, Region::Sdram};
	}

	// ----------------------------------------------------------------- lifetime

	GdbStub::GdbStub(Board& _board) : m_board(_board), m_mcuDriver(*this)
	{
		installWatchers();
	}

	GdbStub::~GdbStub()
	{
		/* The runner is removed before this object dies, so a Scheduler that
		 * outlives the stub goes back to `Board::runMcu` rather than calling
		 * into a destroyed McuDriver. */
		if(m_scheduler != nullptr)
		{
			m_scheduler->setMcuRunner(nullptr);
			m_scheduler = nullptr;
		}

		close();
	}

	void GdbStub::attachScheduler(Scheduler& _scheduler)
	{
		if(m_scheduler == &_scheduler)
			return;

		if(m_scheduler != nullptr)
			m_scheduler->setMcuRunner(nullptr);

		m_scheduler = &_scheduler;
		m_scheduler->setMcuRunner(&m_mcuDriver);
	}

	// ------------------------------------------------------------ the bus hook

	uint32_t GdbStub::Watcher::read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
	{
		m_stub.noteAccess(m_map.window(m_region).base + _offset, _size, false);
		return m_inner.read(_offset, _size, _status);
	}

	void GdbStub::Watcher::write(const uint32_t _offset, const int _size, const uint32_t _value,
	                             mcf5307_bus_status& _status)
	{
		m_stub.noteAccess(m_map.window(m_region).base + _offset, _size, true);
		m_inner.write(_offset, _size, _value, _status);
	}

	/* The access is tested against every watchpoint as a range overlap and not as
	 * an address compare. A four-byte store whose first byte is below the watched
	 * address still touches it, and a debugger that missed that would answer
	 * "nobody writes this address" about an address something writes. */
	void GdbStub::noteAccess(const uint32_t _address, const int _sizeBits, const bool _isWrite)
	{
		if(m_hit.watch)
			return;

		const uint32_t width = _sizeBits == 8 ? 1u : _sizeBits == 16 ? 2u : _sizeBits == 32 ? 4u : 0u;

		if(width == 0u)
			return;

		for(const Watchpoint& point : m_watchpoints)
		{
			if(_isWrite ? !point.onWrite : !point.onRead)
				continue;

			const uint32_t pointEnd  = point.address + point.length;
			const uint32_t accessEnd = _address + width;

			if(_address >= pointEnd || accessEnd <= point.address)
				continue;

			m_hit.watch   = true;
			m_hit.wasRead = !_isWrite;
			m_hit.address = _address;
			return;
		}
	}

	/* Interposed in front of what is already there, and only there. A region with
	 * no target keeps none: attaching a wrapper over nothing would turn an
	 * unmapped region into a mapped one and change what the machine does. */
	void GdbStub::installWatchers()
	{
		for(const Region region : g_regions)
		{
			BusTarget* const inner = m_board.memory().target(region);

			if(!inner)
				continue;

			m_watchers.push_back(std::make_unique<Watcher>(*this, *inner, m_board.memory(), region));
			m_board.memory().attach(region, m_watchers.back().get());
		}
	}

	void GdbStub::removeWatchers()
	{
		for(const std::unique_ptr<Watcher>& watcher : m_watchers)
			m_board.memory().attach(watcher->region(), &watcher->inner());

		m_watchers.clear();
	}

	// -------------------------------------------------------------- the socket

	uint16_t GdbStub::listenOn(const uint16_t _port)
	{
		if(!socketsReady())
			return 0;

		m_listenFd = std::intptr_t(::socket(AF_INET, SOCK_STREAM, 0));
		if(m_listenFd < 0)
			return 0;

		socketSetOption(m_listenFd, SOL_SOCKET, SO_REUSEADDR, 1);

		/* Loopback and never INADDR_ANY. This is an unauthenticated channel with
		 * full read and write access to the emulated machine. */
		sockaddr_in addr{};
		addr.sin_family      = AF_INET;
		addr.sin_port        = htons(_port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		if(::bind(socketArg(m_listenFd), reinterpret_cast<sockaddr*>(&addr),
		          socklen_t(sizeof addr)) != 0 ||
		   ::listen(socketArg(m_listenFd), 1) != 0)
		{
			socketClose(m_listenFd);
			m_listenFd = -1;
			return 0;
		}

		// The port actually bound, which is the one the caller must print when it
		// asked for zero.
		sockaddr_in bound{};
		socklen_t   length = sizeof bound;

		if(::getsockname(socketArg(m_listenFd), reinterpret_cast<sockaddr*>(&bound), &length) != 0)
		{
			socketClose(m_listenFd);
			m_listenFd = -1;
			return 0;
		}

		m_port = ntohs(bound.sin_port);
		return m_port;
	}

	bool GdbStub::waitForClient()
	{
		if(m_listenFd < 0)
			return false;

		m_clientFd = std::intptr_t(::accept(socketArg(m_listenFd), nullptr, nullptr));
		if(m_clientFd < 0)
			return false;

		socketSetOption(m_clientFd, IPPROTO_TCP, TCP_NODELAY, 1);

		m_running = true;
		return true;
	}

	bool GdbStub::sendRaw(const char* _data, const size_t _size)
	{
		size_t sent = 0;

		while(sent < _size)
		{
			const long n = socketSend(m_clientFd, _data + sent, _size - sent);

			if(n <= 0)
				return false;

			sent += size_t(n);
		}

		return true;
	}

	bool GdbStub::sendPacket(const std::string& _payload)
	{
		unsigned sum = 0;
		for(const char c : _payload)
			sum += unsigned(uint8_t(c));

		char tail[4];
		std::snprintf(tail, sizeof tail, "#%02x", sum & 0xffu);

		const std::string out = "$" + _payload + tail;
		return sendRaw(out.data(), out.size());
	}

	/* Read one packet and acknowledge it. Everything ahead of the `$` is
	 * discarded, which is what makes the peer's own `+` and `-` transparent
	 * here.
	 *
	 * A checksum mismatch is not the end of the session. `-` is the
	 * protocol's request to send that packet again, so the debugger answers
	 * it with a retransmission and this loop reads that. Returning false
	 * there would close the session instead: servePacket forwards a false to
	 * serve(), which stops the packet loop, so one corrupted byte would take
	 * the debugger down rather than cost it one round trip. False is reserved
	 * for the client having gone, which is what a recv of anything other
	 * than one byte means. */
	bool GdbStub::readPacket(std::string& _payload)
	{
		for(;;)
		{
			char c = 0;

			for(;;)
			{
				if(socketRecv(m_clientFd, &c, 1) != 1)
					return false;
				if(c == '$')
					break;
			}

			_payload.clear();
			unsigned sum = 0;

			for(;;)
			{
				if(socketRecv(m_clientFd, &c, 1) != 1)
					return false;
				if(c == '#')
					break;
				_payload += c;
				sum += unsigned(uint8_t(c));
			}

			char given[2] = {0, 0};
			if(socketRecv(m_clientFd, &given[0], 1) != 1 || socketRecv(m_clientFd, &given[1], 1) != 1)
				return false;

			char want[3];
			std::snprintf(want, sizeof want, "%02x", sum & 0xffu);

			if(want[0] != given[0] || want[1] != given[1])
			{
				// A NAK, and then this is not a packet. Ask for it again and
				// read the retransmission.
				const char minus = '-';
				if(!sendRaw(&minus, 1))
					return false;
				continue;
			}

			const char plus = '+';
			return sendRaw(&plus, 1);
		}
	}

	// ------------------------------------------------------------ the commands

	/* A plain stop is `T05`; a stop on a watched address names that address, so
	 * the question "who writes this" is answered by the packet itself. */
	std::string GdbStub::stopReply() const
	{
		std::string reply = "T";
		reply += g_sigTrap;

		if(m_hit.watch)
		{
			reply += m_hit.wasRead ? "rwatch:" : "watch:";
			reply += hexAddress(m_hit.address);
			reply += ";";
		}

		return reply;
	}

	/* The register block, big-endian, read from the core on every call. There is
	 * no cached copy and there must not be one: a debugger asks `g` precisely
	 * because the machine has moved, and a copy would answer where it used to
	 * be. */
	std::string GdbStub::readRegisters() const
	{
		std::string reply;

		for(int i = 0; i < g_regCount; ++i)
			reply += hexWord32(m_board.mcuReg(i));

		return reply;
	}

	/* The program counter is read-only through setMcuReg, and that is the core's
	 * rule and not this file's: mcf5307.h states it at index 17. The value is
	 * offered and the core keeps its own, so a `G` carrying a PC is accepted and
	 * changes nothing. */
	std::string GdbStub::writeRegisters(const std::string& _hex)
	{
		if(_hex.size() < size_t(g_regCount) * 8u)
			return g_error;

		for(int i = 0; i < g_regCount; ++i)
		{
			size_t   pos   = 0;
			uint32_t value = 0;

			const std::string field = _hex.substr(size_t(i) * 8u, 8u);

			if(!parseHex(field, pos, value) || pos != field.size())
				return g_error;

			if(i == g_regPc)
				continue;

			(void)m_board.setMcuReg(i, value);
		}

		return "OK";
	}

	/* `m addr,length`. Every byte goes through Board::onRead, which is the exact
	 * callback the core was given, so what a debugger sees is what the machine
	 * sees and not a second reading of the same memory. */
	std::string GdbStub::readMemory(const std::string& _arguments)
	{
		size_t   pos     = 0;
		uint32_t address = 0;
		uint32_t length  = 0;

		if(!parseHex(_arguments, pos, address))
			return g_error;
		if(pos >= _arguments.size() || _arguments[pos] != ',')
			return g_error;
		++pos;
		if(!parseHex(_arguments, pos, length))
			return g_error;

		std::string reply;

		for(uint32_t i = 0; i < length; ++i)
		{
			mcf5307_bus_status status = MCF5307_BUS_OK;
			const uint32_t     value  = Board::onRead(&m_board, address + i, g_byte, &status);

			if(status != MCF5307_BUS_OK)
				return g_error;

			reply += hexByte(uint8_t(value & 0xffu));
		}

		return reply;
	}

	// `M addr,length:<hex bytes>`.
	std::string GdbStub::writeMemory(const std::string& _arguments)
	{
		size_t   pos     = 0;
		uint32_t address = 0;
		uint32_t length  = 0;

		if(!parseHex(_arguments, pos, address))
			return g_error;
		if(pos >= _arguments.size() || _arguments[pos] != ',')
			return g_error;
		++pos;
		if(!parseHex(_arguments, pos, length))
			return g_error;
		if(pos >= _arguments.size() || _arguments[pos] != ':')
			return g_error;
		++pos;

		if(_arguments.size() - pos < size_t(length) * 2u)
			return g_error;

		for(uint32_t i = 0; i < length; ++i)
		{
			unsigned high = 0;
			unsigned low  = 0;

			if(!hexDigit(_arguments[pos + size_t(i) * 2u], high) ||
			   !hexDigit(_arguments[pos + size_t(i) * 2u + 1u], low))
				return g_error;

			mcf5307_bus_status status = MCF5307_BUS_OK;
			Board::onWrite(&m_board, address + i, g_byte, (high << 4) | low, &status);

			if(status != MCF5307_BUS_OK)
				return g_error;
		}

		return "OK";
	}

	/* One instruction and not one cycle. `Board::runMcu(1)` forwards to
	 * `mcf5307_exec(ctx, 1)`, whose loop runs while `spent < maxCycles` -- so a
	 * budget of one executes one whole instruction of whatever cost, and a budget
	 * of zero executes nothing at all. */
	std::string GdbStub::step()
	{
		m_hit = Hit{};

		if(m_scheduler == nullptr)
		{
			(void)m_board.runMcu(1);
			return stopReply();
		}

		/* With a Scheduler, a step is one MCU instruction and one whole quantum.
		 * The allowance of one keeps the MCU to a single instruction; the quantum
		 * around it lets the rest of the machine answer, which is the only reason
		 * a stepped session can cross a host-command handshake at all.
		 *
		 * The bound is not one frame. A quantum whose cycle budget was already
		 * overrun by the previous one runs no MCU cycles at all -- the
		 * long-dispatch branch of `g2::runQuantum` -- so a step that insisted on
		 * exactly one frame would sometimes retire nothing and report a machine
		 * that had not moved. It turns quanta until one instruction has retired. */
		m_allowance = 1;
		driveQuanta(g_stepQuantumBound);
		m_allowance = g_unbounded;

		return stopReply();
	}

	/* The breakpoint compare is an equality. `mcuReg(17)` is read after each
	 * instruction and compared against each armed address with `==`: a breakpoint
	 * stops the machine when the machine is at it, and an address the machine
	 * merely passes is not a stop. A `>=` here would stop at the first
	 * instruction past any armed address, including addresses no program counter
	 * can ever equal. */
	std::string GdbStub::resume()
	{
		m_hit = Hit{};

		if(m_scheduler == nullptr)
		{
			for(uint64_t steps = 0; steps < g_continueBound; ++steps)
			{
				if(m_board.mcuHalted())
					break;

				(void)m_board.runMcu(1);

				if(m_hit.watch)
					break;

				if(m_board.mcuHalted())
					break;

				if(atBreakpoint())
					break;
			}

			return stopReply();
		}

		/* With a Scheduler, a continue turns whole quanta and the breakpoint
		 * compare happens inside each one, in runMcuBudget, after every single
		 * instruction. The DSP advance therefore cannot swallow a hit: the
		 * quantum's MCU phase returns the moment the compare fires, before the
		 * DSPs of that quantum run.
		 *
		 * What it costs is one quantum of skew. The quantum that produced the
		 * stop runs its remaining phases to completion, so the DSP set can be one
		 * block ahead of the MCU at the stop. Returning from the middle of a
		 * quantum would leave the machine in a state the phase order never
		 * produces. */
		m_allowance = g_unbounded;
		driveQuanta(g_continueQuantumBound);

		return stopReply();
	}

	/* The one site that turns the Scheduler: `s` and `c` differ in their
	 * allowance and their bound and in nothing else.
	 *
	 * `m_stop` is how a decision taken mid-quantum gets out. runFrames answers
	 * void and a quantum has no other return path, so the runner records the
	 * stop and this loop reads it after the quantum it happened in. */
	void GdbStub::driveQuanta(const uint64_t _bound)
	{
		m_stop    = false;
		m_retired = 0;

		if(m_board.mcuHalted())
			return;

		for(uint64_t quanta = 0; quanta < _bound; ++quanta)
		{
			m_scheduler->runFrames(1);

			if(m_stop || m_retired >= m_allowance)
				break;
		}
	}

	/* The MCU half of one quantum. The only thing it adds to
	 * `Board::runMcu(_want)` is a decision point between two instructions.
	 *
	 * The return is the cycles actually spent and it may be fewer than `_want`.
	 * That is the ordinary short-spend case `g2::runQuantum` already handles: it
	 * floors the debt at zero and banks no credit, so a quantum cut short by a
	 * breakpoint costs the MCU those cycles and distorts nothing else.
	 *
	 * A zero-cost instruction is counted as one cycle as a termination guarantee
	 * and not as an estimate. The loop's exit condition is a cycle total; a core
	 * that answered zero for an instruction would leave it turning forever. The
	 * retired-instruction count below is exact either way. */
	uint32_t GdbStub::runMcuBudget(const uint32_t _want) noexcept
	{
		/* The watchpoint record is cleared at the top of each quantum's MCU
		 * phase, so that what a stop reply names is an access this phase made.
		 * The bus wrapper reports whoever calls the target it wraps, and the
		 * phases either side of this one -- the panel, the chain, the DSP set --
		 * are not the MCU. Without this, an access made by one of them would be
		 * carried into the next MCU instruction's check and reported as the
		 * reason the machine stopped. */
		m_hit = Hit{};

		uint32_t spent = 0;

		while(spent < _want)
		{
			if(m_board.mcuHalted())
			{
				m_stop = true;
				break;
			}

			if(m_retired >= m_allowance)
				break;

			const uint32_t ran = m_board.runMcu(1);
			++m_retired;
			spent += ran > 0 ? ran : 1;

			if(m_hit.watch)
			{
				m_stop = true;
				break;
			}

			if(m_board.mcuHalted())
			{
				m_stop = true;
				break;
			}

			if(atBreakpoint())
			{
				m_stop = true;
				break;
			}
		}

		return spent;
	}

	/* The compare is an equality and not a `>=`. A breakpoint stops the machine
	 * when the machine is at it, and an address the machine merely passes is not
	 * a stop; a `>=` would stop at the first instruction past any armed address,
	 * including addresses no program counter can ever equal. */
	bool GdbStub::atBreakpoint() const
	{
		const uint32_t pc = m_board.mcuReg(g_regPc);

		for(const uint32_t address : m_breakpoints)
		{
			if(pc == address)
				return true;
		}

		return false;
	}

	/* `Z`/`z` with the type digit already read. Type 0 and type 1 are
	 * breakpoints; 2 is a write watchpoint, 3 a read watchpoint and 4 an access
	 * watchpoint. A type this stub does not implement is answered with the empty
	 * reply, which is how the protocol says "unsupported" -- answering `OK` would
	 * tell a debugger a breakpoint is armed that is not. */
	std::string GdbStub::setPoint(const std::string& _arguments, const bool _insert)
	{
		if(_arguments.empty())
			return g_error;

		const char type = _arguments[0];

		size_t pos = 1;
		if(pos >= _arguments.size() || _arguments[pos] != ',')
			return g_error;
		++pos;

		uint32_t address = 0;
		if(!parseHex(_arguments, pos, address))
			return g_error;
		if(pos >= _arguments.size() || _arguments[pos] != ',')
			return g_error;
		++pos;

		uint32_t kind = 0;
		if(!parseHex(_arguments, pos, kind))
			return g_error;

		if(type == '0' || type == '1')
		{
			if(_insert)
			{
				for(const uint32_t existing : m_breakpoints)
				{
					if(existing == address)
						return "OK";
				}
				m_breakpoints.push_back(address);
				return "OK";
			}

			for(size_t i = 0; i < m_breakpoints.size(); ++i)
			{
				if(m_breakpoints[i] != address)
					continue;

				m_breakpoints.erase(m_breakpoints.begin() + long(i));
				return "OK";
			}

			return "OK";
		}

		if(type != '2' && type != '3' && type != '4')
			return {};

		// The `kind` field of a watchpoint is its length in bytes, which is what
		// makes the range test a range test. A zero length would watch nothing,
		// so it is refused rather than armed.
		if(kind == 0u)
			return g_error;

		if(_insert)
		{
			Watchpoint point;
			point.address = address;
			point.length  = kind;
			point.onWrite = type == '2' || type == '4';
			point.onRead  = type == '3' || type == '4';

			m_watchpoints.push_back(point);
			return "OK";
		}

		for(size_t i = 0; i < m_watchpoints.size(); ++i)
		{
			const Watchpoint& point = m_watchpoints[i];

			if(point.address != address || point.length != kind)
				continue;

			const bool onWrite = type == '2' || type == '4';
			const bool onRead  = type == '3' || type == '4';

			if(point.onWrite != onWrite || point.onRead != onRead)
				continue;

			m_watchpoints.erase(m_watchpoints.begin() + long(i));
			return "OK";
		}

		return "OK";
	}

	std::string GdbStub::handlePacket(const std::string& _payload)
	{
		if(_payload.empty())
			return {};

		const char command = _payload[0];
		const std::string arguments = _payload.substr(1);

		switch(command)
		{
		case '?':
			return stopReply();

		case 'g':
			return readRegisters();

		case 'G':
			return writeRegisters(arguments);

		case 'm':
			return readMemory(arguments);

		case 'M':
			return writeMemory(arguments);

		case 's':
			return step();

		case 'c':
			return resume();

		case 'Z':
			return setPoint(arguments, true);

		case 'z':
			return setPoint(arguments, false);

		case 'D':
			// The detach. The session ends after this reply is sent.
			m_running = false;
			return "OK";

		case 'k':
			// The kill. It carries no reply of its own; the empty one is what the
			// packet layer sends while the session ends.
			m_running = false;
			return {};

		case 'q':
			/* The one capability this stub has. Everything else a debugger asks
			 * about is answered with the empty reply, which is the protocol's own
			 * "I do not implement that" -- and a debugger then falls back to the
			 * base protocol rather than believing a capability that is absent. */
			if(arguments.rfind("Supported", 0) == 0)
				return "PacketSize=1000";
			return {};

		default:
			return {};
		}
	}

	bool GdbStub::servePacket()
	{
		if(!m_running || m_clientFd < 0)
			return false;

		std::string payload;

		if(!readPacket(payload))
			return false;

		return sendPacket(handlePacket(payload));
	}

	void GdbStub::serve()
	{
		while(servePacket())
		{
		}
	}

	void GdbStub::close()
	{
		removeWatchers();

		if(m_clientFd >= 0)
			socketClose(m_clientFd);
		if(m_listenFd >= 0)
			socketClose(m_listenFd);

		m_clientFd = -1;
		m_listenFd = -1;
		m_running  = false;
	}
}
