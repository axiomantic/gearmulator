// The GDB stub drives the whole machine and not the MCU alone. This test boots
// the real CODE_30000400.bin, so it is gated on NMG2_ARTIFACTS and reports NOT
// VERIFIED rather than passing when the artifact is absent.
//
// The defect's signature is an absence and not a crash. A stub that steps
// `Board::runMcu` alone runs no DSP, so the moment the MCU issues a host command
// and busy-waits for a DSP to answer the wait never ends and the `c` runs out its
// own bound with the machine still spinning. The debugger then reports a clean,
// quiet, plausible miss -- a stop reply, no error, and a breakpoint that simply
// "was not reached".
//
// The two cases are a pair and neither means anything alone.
//
//   The known positive, `0x300391E8`. It is reached before the first handshake
//   stall, so it is hit by an MCU-only session too. It is what makes the second
//   case's absence an absence: a run in which both are missed says the fixture
//   or the socket is broken and says nothing about the DSPs.
//
//   The case this file is for, `0x30038D1E`. The patch-compiler prologue
//   download, reached once per DSP on every ordinary boot, and reachable only
//   past the HDI08 handshake at `CVR=0xD6` that the MCU spins on at
//   `0x300505D4`. An MCU-only session cannot reach it at all.
//
// The bound is a failure and never a skip. A stub that cannot cross the
// handshake does not crash -- it spins. A test that merely waited would hang the
// suite, and a test that gave up quietly would pass. The watchdog below prints a
// named reason and exits non-zero, so the expiry is a verdict.
//
// No `assert()` anywhere: every verdict here is a return value, a printed line
// and an exit code, so the check is the same check in every build type.

#include "gatedFixture.h"

#include "../artifactResolver.h"
#include "../board.h"
#include "../dspSet.h"
#include "../executor.h"
#include "../gdbStub.h"
#include "../memoryMap.h"
#include "../scheduler.h"
#include "../status.h"

#include "dsp56kBase/logging.h"

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
#include <cstdlib>
#include <cstring>
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

	std::string hex32(const uint32_t _value)
	{
		char buf[11];
		std::snprintf(buf, sizeof buf, "0x%08x", unsigned(_value));
		return buf;
	}

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		std::cout << (_condition ? "ok   " : "FAIL ") << _what << std::endl;
		if(!_condition)
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

	// ------------------------------------------------------------- the watchdog
	//
	// The expiry is a failure with a named reason. It calls _Exit rather than
	// returning, because the thread it is bounding is inside the emulator and
	// cannot be asked to stop; ctest reads the exit status and 1 is a failure.
	// The name is printed before the exit so the reason survives the process.

	std::atomic<bool>        g_finished{false};
	std::atomic<const char*> g_stage{"before the session opened"};

	constexpr int g_watchdogSeconds = 900;

	void watchdog()
	{
		for(int elapsed = 0; elapsed < g_watchdogSeconds; ++elapsed)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
			if(g_finished.load())
				return;
		}

		std::cout << "FAIL t1_gdb_dsp: the session did not finish within "
		          << g_watchdogSeconds << " seconds, at stage <" << g_stage.load()
		          << ">. THE MACHINE IS BUSY-WAITING ON A DSP THAT NEVER RAN: a "
		             "continue that cannot cross an HDI08 handshake spins until "
		             "its own bound and reports a plausible miss." << std::endl;
		std::cout << "FAIL t1_gdb_dsp: bound expired -- this is a FAILURE and not a skip"
		          << std::endl;
		std::cout.flush();
		std::_Exit(1);
	}

	// ------------------------------------------------------------- the placement
	//
	// The same machine `g2TestConsole --gdb` places: the same image at the same
	// entry, the same vector table, the same reset, the same VBR.

	constexpr uint32_t g_entryPc = 0x30000400u;
	constexpr uint32_t g_entrySp = 0x30400000u;

	constexpr int g_regVbr = 18;
	constexpr int g_regPc  = 17;

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

	/* The known positive. Reached before the first handshake stall, so an
	 * MCU-only session hits it too. */
	constexpr uint32_t g_bpBeforeHandshake = 0x300391E8u;

	/* The case this file is for. The patch-compiler prologue download, past the
	 * handshake. */
	constexpr uint32_t g_bpPastHandshake = 0x30038D1Eu;

	// The MCU spin site. It is not asserted on -- it is reported when the second
	// case misses, because "where did it stop instead" is the whole diagnosis.
	constexpr uint32_t g_knownStallPc = 0x300505D4u;

	// The boot regime produces one real underrun line per frame and hundreds of
	// thousands of frames turn here.
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

	class Ram final : public g2::BusTarget
	{
	public:
		explicit Ram(const size_t _size) : m_bytes(_size, 0u) {}

		uint32_t read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status) override
		{
			if(_size != 8 && _size != 16 && _size != 32)
			{
				_status = MCF5307_BUS_SIZE_ILLEGAL;
				return 0u;
			}

			const uint32_t count = uint32_t(_size) / 8u;
			uint32_t       value = 0u;

			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(_offset) + i;
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

			const uint32_t count = uint32_t(_size) / 8u;

			for(uint32_t i = 0; i < count; ++i)
			{
				const size_t index = size_t(_offset) + i;
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

	private:
		std::vector<uint8_t> m_bytes;
	};

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path, std::ios::binary);
		if(!in)
			return {};
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

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

		return config;
	}

	// ---------------------------------------------------------------- the client
	//
	// The client owns the socket and nothing else, and every Board read happens on
	// the main thread between two answered packets.

	std::string framed(const std::string& _payload)
	{
		unsigned sum = 0;
		for(const char c : _payload)
			sum += unsigned(uint8_t(c));

		char tail[4];
		std::snprintf(tail, sizeof tail, "#%02x", sum & 0xffu);
		return "$" + _payload + tail;
	}

	std::string hexAddr(const uint32_t _value)
	{
		char buf[9];
		std::snprintf(buf, sizeof buf, "%x", unsigned(_value));
		return buf;
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

	// ------------------------------------------------------------------ the body

	bool body(const std::string& _directory)
	{
		const std::vector<uint8_t> code = readFile(_directory + "/CODE_30000400.bin");

		if(code.empty())
		{
			std::cout << "FAIL t1_gdb_dsp: CODE_30000400.bin is empty or unreadable under "
			          << _directory << std::endl;
			return false;
		}

		g2::Board board(makeConfig());
		Ram       ram(g_sdramSize);

		if(!ram.place(g_entryPc - g2::g_sdramBase, code))
		{
			std::cout << "FAIL t1_gdb_dsp: the image does not fit the configured SDRAM window"
			          << std::endl;
			return false;
		}

		{
			std::vector<uint8_t> table(g_vectorTableEntries * 4u);

			for(uint32_t entry = 0; entry < g_vectorTableEntries; ++entry)
			{
				for(uint32_t byte = 0; byte < 4u; ++byte)
					table[entry * 4u + byte] =
						uint8_t((g_vectorHandler >> ((3u - byte) * 8u)) & 0xffu);
			}

			if(!ram.place(g_vectorTableBase - g2::g_sdramBase, table))
			{
				std::cout << "FAIL t1_gdb_dsp: the vector table does not fit the SDRAM window"
				          << std::endl;
				return false;
			}
		}

		board.memory().attach(g2::Region::Sdram, &ram);
		board.resetMcu(g_entrySp, g_entryPc);

		if(!board.setMcuReg(g_regVbr, g_vectorTableBase))
		{
			std::cout << "FAIL t1_gdb_dsp: the core refused VBR at register index " << g_regVbr
			          << std::endl;
			return false;
		}

		/* The Scheduler is declared after the Board so that it is destroyed
		 * before it. */
		g2::SerialExecutor executor;
		g2::Status         schedulerStatus{};

		const std::unique_ptr<g2::Scheduler> scheduler =
			g2::Scheduler::create(g2::Scheduler::Config(), executor, board, schedulerStatus);

		if(!scheduler)
		{
			std::cout << "FAIL t1_gdb_dsp: Scheduler::create returned no object; g2::Status = "
			          << uint32_t(schedulerStatus) << std::endl;
			return false;
		}

		/* The stub is constructed after every unit is attached, because it
		 * interposes its watchpoint wrapper on the targets the map holds at that
		 * moment. */
		g2::GdbStub stub(board);
		g_stub = &stub;

		/* This is the one line that gives the session the whole machine. Without
		 * it the stub steps `Board::runMcu` alone and the later cases all go red
		 * while the known positive stays green, which is the signature of the
		 * defect. */
		stub.attachScheduler(*scheduler);

		const uint16_t port = stub.listenOn(0);
		check(port != 0, "the stub binds a loopback port and reports it");
		if(port == 0)
			return false;

		Channel channel;
		g_channel = &channel;

		Client      client;
		std::thread clientThread;

		{
			bool        connected = false;
			std::thread connector([&] { connected = client.connect(port); });

			const bool accepted = stub.waitForClient();
			connector.join();

			check(connected, "the test client connects to the stub's loopback port");
			check(accepted, "the stub accepts the client");

			if(!connected || !accepted)
				return false;
		}

		clientThread = std::thread(clientLoop, std::ref(channel), std::ref(client));

		checkEqual(board.mcuReg(g_regPc), g_entryPc,
		           "the machine starts at the entry point and has executed nothing");

		/* The baseline is taken before any continue, so the instruction-counter
		 * case compares against the state this session started from and not
		 * against a figure written down here. */
		std::vector<uint64_t> dspBaseline;
		for(unsigned port = 0; port < board.dspSet().dspCount(); ++port)
			dspBaseline.push_back(board.dspSet().dsp(port).getInstructionCounter());

		// The known positive. `0x300391E8` sits before the first handshake stall,
		// so this case passes with the DSPs advancing and without them. It is
		// what makes the next case's absence an absence.
		g_stage = "case one, the known positive at 0x300391E8";
		{
			checkText(ask("Z0," + hexAddr(g_bpBeforeHandshake) + ",2"), "OK",
			          "case one: Z0 is accepted at the pre-handshake routine");
			checkText(ask("c"), "T05", "case one: c answers a stop reply");
			checkEqual(board.mcuReg(g_regPc), g_bpBeforeHandshake,
			           "case one: THE INSTRUMENT CAN SEE -- the machine stopped at the "
			           "pre-handshake breakpoint");
			checkText(ask("z0," + hexAddr(g_bpBeforeHandshake) + ",2"), "OK",
			          "case one: z0 removes it again");
		}

		// The case this file is for. `0x30038D1E` is reachable only past the
		// HDI08 handshake the MCU spins on. A stub that steps the MCU alone runs
		// out its continue bound with the machine still spinning and reports a
		// plausible miss.
		g_stage = "case two, the post-handshake routine at 0x30038D1E";
		{
			checkText(ask("Z0," + hexAddr(g_bpPastHandshake) + ",2"), "OK",
			          "case two: Z0 is accepted at the patch-compiler prologue download");
			checkText(ask("c"), "T05", "case two: c answers a stop reply");

			const uint32_t stopped = board.mcuReg(g_regPc);

			checkEqual(stopped, g_bpPastHandshake,
			           "case two: THE MACHINE CROSSED THE HDI08 HANDSHAKE and stopped at the "
			           "post-handshake breakpoint");

			if(stopped != g_bpPastHandshake)
			{
				std::cout << "     diagnosis: the machine is at " << hex32(stopped)
				          << ", halted=" << (board.mcuHalted() ? 1 : 0)
				          << ", faulted=" << (board.faulted() ? 1 : 0) << std::endl;

				if(stopped == g_knownStallPc)
					std::cout << "     diagnosis: that is the MCU spin on the inverted HDI08 "
					             "ISR byte after CVR=0xD6 -- NO DSP EVER ANSWERED"
					          << std::endl;
			}

			check(!board.mcuHalted(),
			      "case two: a machine stopped at a breakpoint has not halted");
			check(!board.faulted(),
			      "case two: a machine stopped at a breakpoint has not faulted");

			checkText(ask("z0," + hexAddr(g_bpPastHandshake) + ",2"), "OK",
			          "case two: z0 is accepted");
		}

		// The DSPs really ran, read off the DSPs and not inferred from the fact
		// that a breakpoint was hit.
		//
		// The instruction counter is the discriminator and `programLanded` is
		// not: the landed flag is the host-side bridge's, set by the download,
		// and the MCU performs the download on its own, so the flag is set even
		// when no DSP ran. `DSP::exec` is the only thing that moves the
		// instruction counter, and only the Scheduler's DSP phase calls it.
		g_stage = "case three, the DSPs' own instruction counters";
		{
			/* The quanta the session turned are printed, not asserted against a
			 * literal. The figure tells a later reader whether the stub's
			 * continue bound still has room over what a boot actually costs. */
			std::cout << "     measured: the session turned " << scheduler->frameIndex()
			          << " quanta to reach the post-handshake breakpoint" << std::endl;

			check(scheduler->frameIndex() > 0,
			      "case three: the scheduler turned at least one quantum during the session");

			unsigned       ranPorts = 0;
			const unsigned ports    = board.dspSet().dspCount();

			for(unsigned port = 0; port < ports; ++port)
			{
				if(board.dspSet().dsp(port).getInstructionCounter() > dspBaseline[port])
					++ranPorts;
			}

			checkEqual(ranPorts, ports,
			           "case three: EVERY DSP position retired instructions during the session");
		}

		g_stage = "case four, the step model";
		{
			/* One `s` retires one MCU instruction and turns one quantum. Both
			 * halves are asserted, because either alone is satisfied by a stub
			 * with the wrong model: a step that ran a whole quantum's worth of
			 * MCU would move the PC much further, and a step that froze the rest
			 * of the machine would leave the frame index where it was. */
			const uint32_t pcBefore    = board.mcuReg(g_regPc);
			const uint64_t framesBefore = scheduler->frameIndex();

			checkText(ask("s"), "T05", "case four: s answers a stop reply");

			check(board.mcuReg(g_regPc) != pcBefore,
			      "case four: one s moved the program counter");
			checkEqual(uint32_t(scheduler->frameIndex() - framesBefore), 1u,
			           "case four: one s turned exactly one quantum, so the rest of the machine "
			           "advanced by one block and not by none");
		}

		g_stage = "the detach";
		checkText(ask("D"), "OK", "the stub answers the detach packet");

		{
			std::lock_guard<std::mutex> lock(channel.mutex);
			channel.quit = true;
		}
		channel.cv.notify_all();
		clientThread.join();
		client.close();
		stub.close();

		g_stub    = nullptr;
		g_channel = nullptr;

		return g_failures == 0;
	}
}

int main()
{
	installLogFilter();

	std::thread guard(watchdog);
	guard.detach();

	g2::EnvArtifactResolver     resolver;
	g2::test::GatedCounters counters;

	g2::test::runGated(resolver, std::cout, counters, [&]
	{
		std::string       why;
		const std::string directory = resolver.resolve(why, "CODE_30000400.bin");

		if(directory.empty())
		{
			std::cout << "FAIL t1_gdb_dsp: " << why << std::endl;
			++g_failures;
			return false;
		}

		return body(directory);
	});

	g_finished = true;

	std::cout << (g_failures == 0 ? "PASS " : "FAIL ")
	          << (g_cases - g_failures) << "/" << g_cases << " cases" << std::endl;

	std::cout << g2::test::summaryLine(counters) << std::endl;

	return g2::test::gatedExitCode(counters);
}
