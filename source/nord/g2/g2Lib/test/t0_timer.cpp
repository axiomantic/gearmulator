// Task BRD-33. The MCF5307 general-purpose timers.
//
// Plan section 13.1, BRD-33. Plan section 24.6 rows W3-369, W3-377, W3-378.
// Design sections 9.4, 13.1.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG, so a bare assert() is removed and a check
// built on one can never fail. Every case below reports through a counter and
// the process exit status.
//
// WHAT THIS TEST PROVES, AND WHY EACH PART IS HERE.
//
//   1. THE COUNTER COUNTS, AND THE PRESCALER IS PART OF THE COUNT. TMR[15:8]
//      is PS and the counter advances once per PS + 1 input clocks. A model
//      that stored PS and ignored it counts 128 times too fast, so case group
//      2 states the match in INPUT CLOCKS and not in counter ticks.
//
//   2. THE REFERENCE MATCH SETS TER[REF] AND FRR DECIDES WHAT FOLLOWS. The
//      match arrives on the tick whose PRE-INCREMENT counter equals TRR, which
//      is why TRR = 4 takes five ticks and TRR = 1 with PS = 127 takes 128 * 2
//      input clocks. That is the (PS + 1) * (TRR + 1) period the MCF5307 User's
//      Manual states for the timer module. With FRR set the counter restarts
//      from zero and with FRR clear it runs on past TRR.
//
//   3. TER IS WRITE-ONE-TO-CLEAR. The firmware's handler at 0x30001894 writes
//      2 to MBAR+$191, and a model that treated the register as write-any
//      would be cleared by the 0 that the handler never writes and would not be
//      cleared by the 2 that it does.
//
//   4. THE INTERRUPT IS RAISED ONLY WHEN TMR[ORI] IS SET, AND IT IS RAISED
//      THROUGH THE CONTROLLER BRD-3 ALREADY BUILT. The other timer at
//      MBAR+$140 is programmed TMR = 0x2B, whose ORI is CLEAR, so a unit that
//      raised an interrupt regardless of ORI would fabricate one the hardware
//      does not. The assertion runs through a RECORDING DOUBLE -- an
//      InterruptController whose present callback counts calls -- so the silent
//      case fails if the call is made unconditionally.
//
//   5. THE SIM ROUTES THE TEN ADDRESSES TO THE UNITS. Before this task those
//      ten addresses were plain read/write storage: the counter never counted.
//      Case group 7 drives the Sim at its own BusTarget surface and asserts the
//      counter it reads back is the unit's and not a stored byte.
//
//   6. THE BOARD ADVANCES THE TIMERS FROM THE CYCLES IT ACTUALLY RAN. Case
//      group 8 runs the real MCF5307 core over a page of NOPs and asserts the
//      counter equals the cycle count runMcu returned. A Board that advanced
//      nothing -- which is the defect W3-377 measured -- reads zero there.

#include "../board.h"
#include "../interruptController.h"
#include "../sim.h"
#include "../timer.h"

#include <mcf5307.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
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

	// ------------------------------------------------------------ the double
	//
	// The recording double is a real InterruptController with a present
	// callback that COUNTS. A count is what makes the ORI-clear case fail when
	// the raise is made unconditionally: an assertion on the presented level
	// alone would read the same zero whether the unit stayed silent or called
	// setInternalPending for a source whose ICR level is zero.
	struct PresentRecorder
	{
		int calls = 0;
		int level = -999;
		uint8_t vector = 0;
		int autovector = -999;

		void reset()
		{
			calls = 0;
			level = -999;
			vector = 0;
			autovector = -999;
		}
	};

	void recordPresent(void* _user, const int _level, const uint8_t _vector, const int _autovector)
	{
		auto* recorder = static_cast<PresentRecorder*>(_user);
		++recorder->calls;
		recorder->level = _level;
		recorder->vector = _vector;
		recorder->autovector = _autovector;
	}

	// The ICR byte, written out from the manual rather than taken from any
	// header: bit 7 AVEC, IL[2:0] at bits 4:2, IP[1:0] at bits 1:0.
	uint8_t makeIcr(const int _level, const int _ip, const bool _avec)
	{
		uint8_t value = uint8_t((_level << 2) | (_ip & 0x03));
		if(_avec)
			value |= 0x80u;
		return value;
	}

	// The TMR bit positions, written out by hand from MCF5307 UM section 9.4.
	// This file owns its own copy so the two sides move independently.
	constexpr uint16_t kRst = 0x0001u;   // bit 0, timer enable
	constexpr uint16_t kFrr = 0x0008u;   // bit 3, free run / restart
	constexpr uint16_t kOri = 0x0010u;   // bit 4, output reference interrupt
	constexpr uint16_t kClk1 = 0x0002u;  // CLK[1:0] = 01, the master clock

	constexpr uint8_t kRef = 0x02u;      // TER bit 1, the reference event
	constexpr uint8_t kCap = 0x01u;      // TER bit 0, the capture event

	uint16_t makeTmr(const uint16_t _ps, const bool _frr, const bool _ori, const bool _rst)
	{
		uint16_t value = uint16_t(_ps << 8) | kClk1;
		if(_frr) value = uint16_t(value | kFrr);
		if(_ori) value = uint16_t(value | kOri);
		if(_rst) value = uint16_t(value | kRst);
		return value;
	}

	// The two firmware words this task exists for, MEASURED at the end of a
	// real boot and recorded in plan section 24.6 row W3-377.
	constexpr uint16_t kFirmwareTmr2 = 0x7F3Bu;   // PS = 0x7F, FRR set, ORI SET
	constexpr uint16_t kFirmwareTmr1 = 0x002Bu;   // PS = 0,    FRR set, ORI CLEAR
	constexpr uint16_t kFirmwareTrr2 = 0x0032u;

	// The MBAR window this file's Sim and Board cases use. The base is this
	// fixture's own; nothing outside it depends on the number.
	constexpr uint32_t kMbarBase = 0x10000000u;
	constexpr uint32_t kCs0Base  = 0x30000000u;
	constexpr uint32_t kCs0Size  = 0x00001000u;

	constexpr int g_word = 2;   // the core's size unit is BYTES

	uint32_t boardRead(g2::Board& _board, const uint32_t _address, const int _size,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		return g2::Board::onRead(&_board, _address, _size, &_status);
	}

	void boardWrite(g2::Board& _board, const uint32_t _address, const int _size,
		const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;
		g2::Board::onWrite(&_board, _address, _size, _value, &_status);
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 1. THE COUNTER COUNTS, AND THE MATCH ARRIVES ON THE (TRR + 1)th
	// TICK AND NOT ON THE TRRth.
	//
	// PS = 0, so one input clock is one counter tick and the prescaler cannot
	// hide the count. TRR = 4. FOUR advances do not set REF and the FIFTH does.
	{
		g2::Timer timer(g2::Timer::gTimer2InterruptIndex);
		timer.writeTrr(4);
		timer.writeTmr(makeTmr(0, false, false, true));

		timer.advance(4);
		checkEqual(uint32_t(timer.ter() & kRef), uint32_t(0),
			"PS = 0, TRR = 4: four advances do NOT set TER[REF]");
		checkEqual(uint32_t(timer.tcn()), uint32_t(4),
			"PS = 0, TRR = 4: four advances leave the counter at 4");

		timer.advance(1);
		checkEqual(uint32_t(timer.ter() & kRef), uint32_t(kRef),
			"PS = 0, TRR = 4: the FIFTH advance sets TER[REF]");
	}

	// -----------------------------------------------------------------------
	// Case group 2. THE PRESCALER IS PART OF THE COUNT.
	//
	// PS = 127 and TRR = 1, which is the firmware's prescaler. The match
	// arrives after 128 * 2 INPUT CLOCKS and not after 2. A model that stored
	// PS and divided by nothing sets REF on the second input clock, which is
	// the required-RED this case names.
	{
		g2::Timer timer(g2::Timer::gTimer2InterruptIndex);
		timer.writeTrr(1);
		timer.writeTmr(makeTmr(127, false, false, true));

		timer.advance(2);
		checkEqual(uint32_t(timer.ter() & kRef), uint32_t(0),
			"PS = 127, TRR = 1: two input clocks do NOT set TER[REF]");
		checkEqual(uint32_t(timer.tcn()), uint32_t(0),
			"PS = 127, TRR = 1: two input clocks do not even move the counter");

		timer.advance(128 * 2 - 2 - 1);
		checkEqual(uint32_t(timer.ter() & kRef), uint32_t(0),
			"PS = 127, TRR = 1: 128 * 2 - 1 input clocks do NOT set TER[REF]");

		timer.advance(1);
		checkEqual(uint32_t(timer.ter() & kRef), uint32_t(kRef),
			"PS = 127, TRR = 1: TER[REF] is set on input clock 128 * 2 exactly");
	}

	// -----------------------------------------------------------------------
	// Case group 3. FRR DECIDES WHAT THE COUNTER DOES AFTER THE MATCH.
	//
	// The two halves are the same run with one bit exchanged. A model that
	// ignored FRR reads the same counter in both halves, and one of the two
	// halves is then red whichever way it was written.
	{
		g2::Timer restart(g2::Timer::gTimer2InterruptIndex);
		restart.writeTrr(4);
		restart.writeTmr(makeTmr(0, true, false, true));
		restart.advance(5);

		checkEqual(uint32_t(restart.ter() & kRef), uint32_t(kRef),
			"FRR set: the match sets TER[REF]");
		checkEqual(uint32_t(restart.tcn()), uint32_t(0),
			"FRR SET: the counter reads ZERO after the match");

		restart.advance(5);
		checkEqual(uint32_t(restart.tcn()), uint32_t(0),
			"FRR set: the next period is TRR + 1 ticks long, so the counter reads zero again");

		g2::Timer freeRun(g2::Timer::gTimer2InterruptIndex);
		freeRun.writeTrr(4);
		freeRun.writeTmr(makeTmr(0, false, false, true));
		freeRun.advance(5);

		checkEqual(uint32_t(freeRun.ter() & kRef), uint32_t(kRef),
			"FRR clear: the match sets TER[REF]");
		checkEqual(uint32_t(freeRun.tcn()), uint32_t(5),
			"FRR CLEAR: the counter RUNS ON past TRR and reads 5");

		freeRun.advance(3);
		checkEqual(uint32_t(freeRun.tcn()), uint32_t(8),
			"FRR clear: the counter keeps running on past TRR");
	}

	// -----------------------------------------------------------------------
	// Case group 4. TER IS WRITE-ONE-TO-CLEAR AND NOT WRITE-ANY.
	//
	// The firmware's handler writes 2 to MBAR+$191. A write-any model is
	// cleared by the 0 the handler never writes, and is SET to 0 by the 2 it
	// does -- which happens to look the same for that one value, so both halves
	// are needed: the 0 must NOT clear and the 2 MUST.
	{
		g2::Timer timer(g2::Timer::gTimer2InterruptIndex);
		timer.writeTrr(0);
		timer.writeTmr(makeTmr(0, true, false, true));
		timer.advance(1);

		checkEqual(uint32_t(timer.ter()), uint32_t(kRef),
			"the match sets TER[REF] and nothing else");

		timer.writeTer(0);
		checkEqual(uint32_t(timer.ter()), uint32_t(kRef),
			"WRITE-ONE-TO-CLEAR: a write of 0 to TER does NOT clear REF");

		timer.writeTer(kCap);
		checkEqual(uint32_t(timer.ter()), uint32_t(kRef),
			"write-one-to-clear: a write of the CAP bit does not clear REF");

		timer.writeTer(kRef);
		checkEqual(uint32_t(timer.ter()), uint32_t(0),
			"write-one-to-clear: the write of 2 the firmware's handler makes DOES clear REF");
	}

	// -----------------------------------------------------------------------
	// Case group 5. THE INTERRUPT IS RAISED ONLY WHEN TMR[ORI] IS SET.
	//
	// Both halves run the FIRMWARE'S OWN TMR words. Timer 2's 0x7F3B has ORI
	// set and timer 1's 0x2B has ORI clear, and the second half is the one that
	// fails when the raise is made unconditionally.
	{
		PresentRecorder recorder;
		g2::InterruptController controller(&recorder, recordPresent);

		// Timer 2 is internal source 2 (ICR2 at MBAR+$04E), which is the ICR
		// the firmware programs. Level 1, autovectored, as ICR = 0x84 selects.
		controller.writeRegister(0x04Cu + g2::Timer::gTimer2InterruptIndex, makeIcr(1, 0, true));
		controller.writeRegister(0x04Cu + g2::Timer::gTimer1InterruptIndex, makeIcr(1, 0, true));

		g2::Timer timer2(g2::Timer::gTimer2InterruptIndex, &controller);
		timer2.writeTrr(kFirmwareTrr2);
		timer2.writeTmr(kFirmwareTmr2);

		recorder.reset();
		timer2.advance(128 * (0x32 + 1));

		checkEqual(uint32_t(timer2.ter() & kRef), uint32_t(kRef),
			"ORI set: the firmware's TMR = 0x7F3B and TRR = 0x32 reach a reference match after 128 * 51 input clocks");
		checkEqual(recorder.calls, 1,
			"ORI SET: the match raises the interrupt through the controller, exactly once");
		checkEqual(recorder.level, 1,
			"ORI set: the level presented is the level ICR2 carries");
		checkEqual(recorder.autovector, 1,
			"ORI set: the interrupt is autovectored, as ICR2 = 0x84 selects");

		// The handler's write clears REF, and the level source drops with it.
		recorder.reset();
		timer2.writeTer(kRef);
		checkEqual(recorder.calls, 1,
			"clearing TER[REF] drops the level source, so the controller is told once");
		checkEqual(recorder.level, 0,
			"with REF cleared the controller presents no interrupt");

		// THE SILENT TIMER. TMR = 0x2B has ORI CLEAR.
		PresentRecorder silent;
		g2::InterruptController quiet(&silent, recordPresent);
		quiet.writeRegister(0x04Cu + g2::Timer::gTimer1InterruptIndex, makeIcr(1, 0, true));

		g2::Timer timer1(g2::Timer::gTimer1InterruptIndex, &quiet);
		timer1.writeTrr(4);
		timer1.writeTmr(kFirmwareTmr1);

		silent.reset();
		timer1.advance(5);

		checkEqual(uint32_t(timer1.ter() & kRef), uint32_t(kRef),
			"ORI clear: the match still sets TER[REF]");
		checkEqual(silent.calls, 0,
			"ORI CLEAR: the timer the firmware programs with TMR = 0x2B raises NO interrupt");
	}

	// -----------------------------------------------------------------------
	// Case group 6. TMR[RST] IS THE ENABLE AND A DISABLED TIMER DOES NOT COUNT.
	//
	// MCF5307 UM section 9.4: RST = 0 disables the timer and resets it. Without
	// this the two timers would count from the machine's first cycle, before
	// the firmware has programmed either of them.
	{
		g2::Timer timer(g2::Timer::gTimer2InterruptIndex);
		timer.writeTrr(4);
		timer.writeTmr(makeTmr(0, true, false, false));   // RST clear

		timer.advance(100);
		checkEqual(uint32_t(timer.tcn()), uint32_t(0),
			"RST clear: a disabled timer does not count");
		checkEqual(uint32_t(timer.ter() & kRef), uint32_t(0),
			"RST clear: a disabled timer reaches no reference match");

		timer.writeTmr(makeTmr(0, true, false, true));    // RST set
		timer.advance(3);
		checkEqual(uint32_t(timer.tcn()), uint32_t(3),
			"RST set: the timer counts from zero once it is enabled");

		timer.writeTmr(makeTmr(0, true, false, false));   // RST clear again
		checkEqual(uint32_t(timer.tcn()), uint32_t(0),
			"clearing RST resets the counter");
	}

	// -----------------------------------------------------------------------
	// Case group 7. THE SIM ROUTES THE TEN ADDRESSES TO THE UNITS.
	//
	// The Sim is driven at its own BusTarget surface, at the MBAR-relative
	// offsets the manual assigns. Before this task every one of these ten
	// addresses was plain storage, so the counter read back what was last
	// written and never anything else.
	{
		g2::Sim sim;
		mcf5307_bus_status status = MCF5307_BUS_OK;

		checkEqual(sim.read(0x184, 16, status), uint32_t(0xffffu),
			"TRR2 reads its reset value 0xffff through the Sim");

		sim.write(0x184, 16, kFirmwareTrr2, status);
		sim.write(0x180, 16, kFirmwareTmr2, status);

		checkEqual(sim.read(0x180, 16, status), uint32_t(kFirmwareTmr2),
			"TMR2 reads back the firmware's 0x7f3b through the Sim");
		checkEqual(sim.read(0x184, 16, status), uint32_t(kFirmwareTrr2),
			"TRR2 reads back the firmware's 0x32 through the Sim");
		checkEqual(sim.read(0x18c, 16, status), uint32_t(0),
			"TCN2 starts at zero");

		sim.advanceTimers(128 * 10);
		checkEqual(sim.read(0x18c, 16, status), uint32_t(10),
			"THE COUNTER COUNTS THROUGH THE SIM: 128 * 10 input clocks with PS = 0x7f leave TCN2 at 10");
		checkEqual(sim.read(0x191, 8, status), uint32_t(0),
			"TER2 is still clear ten ticks into a 51-tick period");

		sim.advanceTimers(128 * (0x32 + 1 - 10));
		checkEqual(sim.read(0x191, 8, status), uint32_t(kRef),
			"the reference match sets TER2[REF] through the Sim");
		checkEqual(sim.read(0x18c, 16, status), uint32_t(0),
			"FRR is set in 0x7f3b, so TCN2 restarts at zero");

		sim.write(0x191, 8, 0x00u, status);
		checkEqual(sim.read(0x191, 8, status), uint32_t(kRef),
			"through the Sim, a write of 0 to MBAR+0x191 does not clear REF");
		sim.write(0x191, 8, kRef, status);
		checkEqual(sim.read(0x191, 8, status), uint32_t(0),
			"through the Sim, the firmware handler's write of 2 to MBAR+0x191 clears REF");

		// The other unit is a SEPARATE unit and shares no state with it.
		checkEqual(sim.read(0x14c, 16, status), uint32_t(0),
			"TCN1 is a separate counter and stayed at zero while timer 2 ran");

		// TCR is read-only on both units. UM Table B-1.
		sim.write(0x188, 16, 0xffffu, status);
		checkEqual(sim.read(0x188, 16, status), uint32_t(0),
			"TCR2 is read-only and a write reaches no bit of it");
	}

	// -----------------------------------------------------------------------
	// Case group 8. THE BOARD ADVANCES THE TIMERS FROM THE CYCLES IT RAN.
	//
	// The real MCF5307 core runs a page of NOPs out of CS0 and the counter is
	// read back through the same bus callbacks the core uses. THE ASSERTION IS
	// AN EQUALITY AGAINST THE CYCLE COUNT runMcu RETURNED, so a Board that
	// advanced the timers from anything other than the cycles it ran is red
	// here, and a Board that advanced nothing -- the defect row W3-377
	// measured -- reads zero.
	{
		g2::BoardConfig config;
		config.memory.cs0 = {kCs0Base, kCs0Size};
		config.memory.mbar = {kMbarBase, 0x400u};

		g2::Board board(config);

		// 0x4E71 is NOP. The whole window is NOPs, so the core cannot leave it
		// inside the budget below.
		std::vector<uint8_t> nops(kCs0Size);
		for(size_t i = 0; i < nops.size(); i += 2)
		{
			nops[i]     = 0x4Eu;
			nops[i + 1] = 0x71u;
		}
		board.flash().loadCs0(nops);

		mcf5307_bus_status status = MCF5307_BUS_OK;

		// PS = 0 so one cycle is one tick, FRR set, ORI CLEAR so nothing is
		// raised, RST set so the timer is enabled.
		boardWrite(board, kMbarBase + 0x184, g_word, 0xffffu, status);
		boardWrite(board, kMbarBase + 0x180, g_word, makeTmr(0, true, false, true), status);

		checkEqual(boardRead(board, kMbarBase + 0x18c, g_word, status), uint32_t(0),
			"through the board: TCN2 is zero before the core has run");

		board.resetMcu(kCs0Base + kCs0Size, kCs0Base);
		const uint32_t ran = board.runMcu(64);

		check(!board.faulted(),
			"through the board: the core ran a page of NOPs without faulting");
		check(ran > 0,
			"through the board: the core reported a non-zero cycle count");
		checkEqual(boardRead(board, kMbarBase + 0x18c, g_word, status), ran,
			"THE BOARD ADVANCES THE TIMERS FROM THE CYCLES IT RAN: TCN2 equals runMcu's return");

		const uint32_t ranAgain = board.runMcu(64);
		checkEqual(boardRead(board, kMbarBase + 0x18c, g_word, status), ran + ranAgain,
			"through the board: a second quantum adds its own cycles to the same counter");
	}

	if(g_failures)
	{
		std::cout << "t0_timer: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_timer: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
