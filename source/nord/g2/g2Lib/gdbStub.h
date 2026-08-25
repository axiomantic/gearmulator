// Task TOOL-13. A GDB remote stub on the MCF5307.
//
// Plan section 12 (TOOL-13). Design sections 18.3, 23.1.1.
//
// WHAT THIS FILE IS. A GDB remote-serial-protocol server that answers a
// debugger over a loopback socket and drives the machine through the surface
// the `Board` already publishes. It exists because seven defects of 2026-08-23
// to 2026-08-25 were each located by adding a `std::cout`, rebuilding, reading
// one line and reverting, at roughly ninety seconds a cycle -- and two of them
// spent an image-wide `grep` plus a disassembly pass answering "who writes this
// address", which is one `watch` command.
//
// IT OWNS NO EMULATOR STATE, AND THAT IS A PROPERTY AND NOT AN INTENTION.
// Nothing in `g2Lib` gains a debug member for this file: the stub holds a
// reference to a `Board` it did not create, and it holds its breakpoints and its
// watchpoints in its OWN containers. A build without this translation unit is
// byte-identical to the build without this task.
//
// WHERE EACH CAPABILITY COMES FROM, so that nobody re-derives it:
//
//   REGISTERS  `mcf5307.h` documents the register file as 0..7 = d0..d7,
//              8..15 = a0..a7, 16 = SR, 17 = PC, which is EXACTLY GDB's m68k
//              order -- so `g` and `G` are a direct eighteen-register
//              big-endian serialisation with no remapping. They reach the
//              machine through `Board::mcuReg` and `Board::setMcuReg`.
//
//   MEMORY     `Board::onRead` and `Board::onWrite` are the exact callbacks the
//              core was given, so `m` and `M` take the path the core takes.
//
//   EXECUTION  `Board::runMcu(1)` forwards to `mcf5307_exec(ctx, 1)`, whose
//              loop runs while `spent < maxCycles` -- so a budget of one runs
//              exactly ONE INSTRUCTION, whatever that instruction costs. That
//              is the single-step primitive. A budget of ZERO runs nothing.
//              THE RETURN IS THAT INSTRUCTION'S WHOLE COST AND NOT THE
//              BUDGET, and the stub reads neither: `step` and `resume` both
//              discard it and report the machine's state instead. Nothing
//              here depends on the number, so the step primitive is exactly
//              what it was.
//
//   HALT       `Board::mcuHalted` and `Board::faulted`.
//
// BREAKPOINTS ARE A PROGRAM-COUNTER COMPARE AND WATCHPOINTS ARE A BUS HOOK.
// `Z0`/`z0` add and remove an address the step loop compares `mcuReg(17)`
// against, with `==` and not with `>=`: a stop is an EQUALITY and an address
// the machine steps over is not a stop. `Z2`/`z2`/`Z3`/`z3` add and remove an
// address range that a wrapper placed in front of the memory map's own targets
// tests on every access. The wrapper is how the stub sees a write without the
// `Board` gaining a member: `MemoryMap::target` hands out what is attached and
// `MemoryMap::attach` puts something else there, so the stub interposes itself
// and forwards. The originals are restored when the stub is destroyed.
//
// THE STUB IS OPT-IN AND ABSENT BY DEFAULT. `g2TestConsole --gdb <port>` starts
// it; no test enables it, and `t1_boot` and every T0 check run exactly as they
// do today. A debugger that changed timing when nobody was debugging would
// invalidate the suite it exists to serve.
//
// LOOPBACK ONLY. The listening socket binds INADDR_LOOPBACK and never
// INADDR_ANY. This is an unauthenticated command channel with full read and
// write access to the emulated machine; it must not be reachable from another
// host, and the bind is where that is decided.
//
// WHAT IT IS NOT. There is NO DSP56300 STUB -- that core has no stock GDB
// target and would need its own register map and target description, which is a
// second task and not a wider version of this one. There is no symbol loading
// and no source-level anything: the firmware is a stripped binary.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "board.h"
#include "memoryMap.h"

namespace g2
{
	class GdbStub final
	{
	public:
		/* The stub borrows the Board and never destroys it. CONSTRUCT IT AFTER
		 * EVERY UNIT IS ATTACHED: the watchpoint wrapper is interposed here, in
		 * front of whatever the memory map holds at this moment, and a target
		 * attached later would sit in front of the wrapper rather than behind
		 * it. */
		explicit GdbStub(Board& _board);
		~GdbStub();

		GdbStub(const GdbStub&)            = delete;
		GdbStub& operator=(const GdbStub&) = delete;
		GdbStub(GdbStub&&)                 = delete;
		GdbStub& operator=(GdbStub&&)      = delete;

		/* Bind and listen on 127.0.0.1. A port of zero asks the operating system
		 * for a free one, which is what the check uses so that two runs cannot
		 * collide. Returns the port actually bound, and ZERO on failure -- the
		 * caller has no listening socket in that case and nothing else here will
		 * answer. */
		uint16_t listenOn(uint16_t _port);

		uint16_t port() const { return m_port; }

		/* Blocks in accept() until a debugger attaches. FALSE when there is no
		 * listening socket or the accept failed. */
		bool waitForClient();

		/* Read one packet, answer it, and return TRUE. FALSE when the client has
		 * gone, when the packet loop was told to stop (`k`), or when there is no
		 * session. It is public because the check drives the stub from its own
		 * thread one packet at a time: that is what keeps every Board read on
		 * one thread, between two answered packets, rather than racing a server
		 * loop. */
		bool servePacket();

		// The packet loop. Returns when servePacket does.
		void serve();

		// Close both sockets. Idempotent.
		void close();

	private:
		/* THE WATCHPOINT WRAPPER. One of these is interposed in front of each
		 * target the memory map already holds, and it forwards every access to
		 * that target unaltered. The offset it receives is window-relative, so it
		 * carries the window base to report an ABSOLUTE address -- taken from the
		 * map rather than copied, for the reason board.h's FlashWindow gives:
		 * two copies of one base agree with each other through any mutation of
		 * either. */
		class Watcher final : public BusTarget
		{
		public:
			Watcher(GdbStub& _stub, BusTarget& _inner, const MemoryMap& _map, const Region _region)
				: m_stub(_stub), m_inner(_inner), m_map(_map), m_region(_region) {}

			uint32_t read(uint32_t _offset, int _size, mcf5307_bus_status& _status) override;
			void write(uint32_t _offset, int _size, uint32_t _value, mcf5307_bus_status& _status) override;

			BusTarget& inner() const { return m_inner; }
			Region     region() const { return m_region; }

		private:
			GdbStub&         m_stub;
			BusTarget&       m_inner;
			const MemoryMap& m_map;
			Region           m_region;
		};

		struct Watchpoint
		{
			uint32_t address = 0;
			uint32_t length  = 0;
			bool     onRead  = false;
			bool     onWrite = false;
		};

		// What the machine stopped for. `None` is the machine still running.
		struct Hit
		{
			bool     watch      = false;
			bool     wasRead    = false;
			uint32_t address    = 0;
		};

		// The wrapper's report, called from Watcher::read and Watcher::write.
		void noteAccess(uint32_t _address, int _sizeBits, bool _isWrite);

		void installWatchers();
		void removeWatchers();

		// The packet layer.
		bool        readPacket(std::string& _payload);
		bool        sendPacket(const std::string& _payload);
		bool        sendRaw(const char* _data, size_t _size);
		std::string handlePacket(const std::string& _payload);

		// The commands.
		std::string readRegisters() const;
		std::string writeRegisters(const std::string& _hex);
		std::string readMemory(const std::string& _arguments);
		std::string writeMemory(const std::string& _arguments);
		std::string step();
		std::string resume();
		std::string setPoint(const std::string& _arguments, bool _insert);

		std::string stopReply() const;

		Board& m_board;

		int      m_listenFd = -1;
		int      m_clientFd = -1;
		uint16_t m_port     = 0;

		bool m_running = false;

		std::vector<uint32_t>   m_breakpoints;
		std::vector<Watchpoint> m_watchpoints;

		std::vector<std::unique_ptr<Watcher>> m_watchers;

		Hit m_hit;
	};
}
