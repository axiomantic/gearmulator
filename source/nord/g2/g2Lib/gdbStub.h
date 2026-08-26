// A GDB remote-serial-protocol server that answers a debugger over a loopback
// socket and drives the machine through the surface the `Board` already
// publishes.
//
// The stub holds a reference to a `Board` it did not create, and it holds its
// breakpoints and its watchpoints in its own containers. A build without this
// translation unit is byte-identical to the build without it.
//
// `mcf5307.h` documents the register file as 0..7 = d0..d7, 8..15 = a0..a7,
// 16 = SR, 17 = PC, which is exactly GDB's m68k order -- so `g` and `G` are a
// direct eighteen-register big-endian serialisation with no remapping. They
// reach the machine through `Board::mcuReg` and `Board::setMcuReg`.
//
// IT OWNS NO EMULATOR STATE, AND THAT IS A PROPERTY AND NOT AN INTENTION.
// Nothing in `g2Lib` gains a debug member for this file: the stub holds a
// reference to a `Board` it did not create, and it holds its breakpoints and its
// watchpoints in its OWN containers.
//
// ONE EXCEPTION, AND IT IS NAMED RATHER THAN LEFT FOR A READER TO FIND. The
// amendment below gave `Scheduler` a borrowed `McuRunner*`, null in every build
// that is not being debugged, costing one null test for each quantum -- which is
// the arrangement `Scheduler::TraceSink` already established. Nothing else in
// `g2Lib` knows this file exists, and a machine with no stub attached runs the
// same quantum it ran before.
//
// `Board::runMcu(1)` forwards to `mcf5307_exec(ctx, 1)`, whose loop runs while
// `spent < maxCycles` -- so a budget of one runs exactly one instruction,
// whatever that instruction costs. A budget of zero runs nothing.
//
// `Z0`/`z0` add and remove an address the step loop compares `mcuReg(17)`
// against, with `==` and not with `>=`: a stop is an equality and an address
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
//
// ====================================================================
// THE AMENDMENT: THE SESSION DRIVES THE WHOLE MACHINE, NOT THE MCU ALONE.
// ====================================================================
//
// THE DEFECT THIS CLOSES HAS AN ABSENCE FOR A SYMPTOM. The first revision
// stepped `Board::runMcu` and nothing else. The moment the MCU issued a host
// command and busy-waited for a DSP to answer -- which is what it does on every
// ordinary boot, at `0x300505D4`, spinning on an inverted HDI08 ISR byte after
// `CVR=0xD6` -- no DSP ever ran, the wait never ended, and the `c` ran out its
// own bound with the machine still spinning. THE DEBUGGER THEN REPORTED A CLEAN,
// QUIET, PLAUSIBLE MISS. A breakpoint on any DSP-interacting routine said "not
// reached" when the truth was "never got there". A pass of 2026-08-26 took
// exactly that reading against `0x30038D1E`, recognised it as unsound and threw
// it away; the reading itself was indistinguishable from a real negative.
//
// THE FULL-ADVANCE PATH IS THE SCHEDULER'S AND THIS FILE DOES NOT WRITE A
// SECOND ONE. `Scheduler::runFrames` is the only site of design section 13.5's
// quantum order -- swap, ingress, panel, SOF, MCU, the eight DSPs, egress -- and
// a debugger carrying its own copy of that order would be a second full-advance
// path that nothing keeps in step with the first. What the stub adds is the ONE
// thing a quantum does not offer: a decision point BETWEEN two MCU instructions,
// where a breakpoint compare has to happen. `Scheduler::McuRunner` is that point
// and it is nothing else. The stub installs itself as the runner; the runner
// steps `Board::runMcu(1)` up to the quantum's own want, checking breakpoints and
// watchpoints after each instruction, and answers with the cycles it spent.
//
// WHAT A `c` MEANS NOW. The whole machine advances, quantum by quantum, until a
// breakpoint, a watchpoint, the machine's own halt or the bound. A breakpoint is
// still an EQUALITY on the MCU program counter and it is still tested after
// every single instruction, so the DSP advance cannot swallow or delay a hit.
// ONE SKEW IS STATED RATHER THAN HIDDEN: the quantum in which the stop happens
// runs its REMAINING phases to completion, so the DSP set can be up to one
// quantum ahead of the MCU at the stop. Stopping mid-quantum would leave the
// machine in a state the scheduler's own order never produces, which is a worse
// lie than a bounded, named skew.
//
// WHAT AN `s` MEANS NOW, AND WHY IT IS THE SMALLER OF TWO DISTORTIONS. One `s`
// retires exactly ONE MCU instruction and turns ONE whole quantum, so the DSP
// set, the chain and the panel each advance by one block. THAT IS NOT
// TIMING-FAITHFUL and the number is not close: a quantum's MCU budget is
// thousands of cycles and a step spends one instruction's worth, and the cycle
// debt rule banks no credit for the shortfall. It is chosen anyway, over the
// alternative of freezing everything but the MCU, for two reasons. FIRST, BOTH
// CHOICES DISTORT RATE AND ONLY ONE IS BOUNDED: freezing the DSPs distorts it
// without limit, because no amount of stepping ever advances them, so a stepped
// session can never cross the handshake this amendment exists to cross. SECOND,
// IT IS THE DIRECTION THE HARDWARE ITSELF GOES: halting one core at a debugger
// prompt does not halt the other nine, and a human at that prompt lets them
// free-run for millions of cycles. One quantum is conservative beside that.
// A QUESTION ABOUT TIMING IS THEREFORE A QUESTION FOR `c` AND A BREAKPOINT, AND
// NOT FOR A STEPPING SESSION.
//
// NO SCHEDULER, NO CHANGE. `attachScheduler` is what turns any of this on. A
// stub constructed against a Board alone behaves exactly as the first revision
// did -- `s` steps the MCU, `c` steps the MCU -- which is what every existing
// check of this file drives and what a caller with no Scheduler to give still
// gets.
//
// BREAKPOINTS REMAIN MCU-SIDE ONLY. The DSPs now RUN; they are not
// INSTRUMENTED. There is still no way to break on a DSP56300 program counter,
// to read a DSP register through this session, or to watch a DSP memory
// address, and none of that is a wider version of this task.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "board.h"
#include "memoryMap.h"
#include "scheduler.h"

namespace g2
{
	class GdbStub final
	{
	public:
		/* The stub borrows the Board and never destroys it. Construct it after
		 * every unit is attached: the watchpoint wrapper is interposed here, in
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
		/* GIVE THE SESSION THE WHOLE MACHINE. Without this the stub drives
		 * `Board::runMcu` and nothing else, which is what the file header calls
		 * out as the defect: a breakpoint past an HDI08 handshake then reports a
		 * clean, plausible MISS rather than being reached.
		 *
		 * IT MAY BE CALLED ONCE AND THE STUB DOES NOT OWN THE SCHEDULER. The
		 * stub installs itself as that Scheduler's `McuRunner` and removes
		 * itself in its destructor, so the Scheduler must outlive the stub --
		 * which the declaration order every caller already uses gives for free,
		 * the Scheduler being declared before the stub and destroyed after it.
		 *
		 * IT DOES NOT ADVANCE THE MACHINE. Nothing here runs a quantum; the
		 * debugger's own `s` and `c` are still what run it. */
		void attachScheduler(Scheduler& _scheduler);

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

		/* THE MCU RUNNER THE SCHEDULER CALLS INSTEAD OF `Board::runMcu`. It is a
		 * separate object rather than a base of GdbStub so that the stub's
		 * public surface does not grow a `runMcu` the protocol layer could call
		 * by mistake -- the same arrangement Watcher above uses for the bus. */
		class McuDriver final : public McuRunner
		{
		public:
			explicit McuDriver(GdbStub& _stub) : m_stub(_stub) {}

			uint32_t runMcu(uint32_t _want) noexcept override { return m_stub.runMcuBudget(_want); }

		private:
			GdbStub& m_stub;
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

		/* THE MCU HALF OF ONE QUANTUM, INSTRUCTION BY INSTRUCTION. Called by
		 * McuDriver with the want the cycle-debt block computed, it steps
		 * `Board::runMcu(1)` until it has spent that many cycles, until the
		 * instruction allowance runs out, or until a stop condition fires, and
		 * answers the cycles it actually spent. */
		uint32_t runMcuBudget(uint32_t _want) noexcept;

		// TRUE when the MCU program counter equals an armed breakpoint address.
		bool atBreakpoint() const;

		/* Drive whole quanta until `m_stop`, the machine's halt or `_bound`.
		 * The one site that turns the Scheduler, used by both `s` and `c`. */
		void driveQuanta(uint64_t _bound);
		std::string setPoint(const std::string& _arguments, bool _insert);

		std::string stopReply() const;

		Board& m_board;

		/* A socket handle, and it is intptr_t and not int because Winsock's
		 * SOCKET is a UINT_PTR. INVALID_SOCKET is all bits set, which is
		 * exactly -1 in this type, so the invalid value and the `< 0` test
		 * that spells it are the same on both platforms. */
		using SocketHandle = std::intptr_t;

		SocketHandle m_listenFd = -1;
		SocketHandle m_clientFd = -1;
		uint16_t     m_port     = 0;

		bool m_running = false;

		std::vector<uint32_t>   m_breakpoints;
		std::vector<Watchpoint> m_watchpoints;

		std::vector<std::unique_ptr<Watcher>> m_watchers;

		Hit m_hit;

		/* NULL UNTIL attachScheduler, AND THAT IS THE WHOLE SWITCH between the
		 * MCU-only session the first revision served and the whole-machine one
		 * this file now serves. */
		Scheduler* m_scheduler = nullptr;

		McuDriver m_mcuDriver;

		/* SET BY runMcuBudget WHEN THE MACHINE SHOULD STOP -- a breakpoint, a
		 * watchpoint or the machine's own halt. It is what carries that decision
		 * out of the middle of a quantum, which has no other return path. */
		bool m_stop = false;

		/* THE INSTRUCTIONS ONE DRIVE MAY RETIRE. Unbounded for a `c`, ONE for an
		 * `s`, and it is what makes a step a step rather than a whole quantum of
		 * MCU progress.
		 *
		 * IT RESTS AT UNBOUNDED AND NOT AT ZERO. The runner stays installed for
		 * the stub's whole life, so a Scheduler driven by anything other than a
		 * debugger command must still run the MCU's whole want; a resting
		 * allowance of zero would silently starve the MCU of every quantum such
		 * a caller turned. */
		uint64_t m_allowance = ~uint64_t(0);

		// Retired by the current drive, against the allowance.
		uint64_t m_retired = 0;
	};
}
