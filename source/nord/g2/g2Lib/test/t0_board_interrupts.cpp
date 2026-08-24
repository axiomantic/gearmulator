// Task BRD-34. The Board owns the interrupt controller and presents to the
// core.
//
// Plan section 13.4, BRD-34. Plan section 24.6 rows W3-379, W3-380.
// Design sections 5.2.2, 6.4, 9.4.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG, so a bare assert() is removed and a check
// built on one can never fail. Every case below reports through a counter and
// the process exit status.
//
// WHAT THIS TEST PROVES, AND WHY IT IS AT THE BOARD TIER AND NOT THE CLASS
// TIER. t0_interrupts already drives InterruptController directly and proves
// the arbitration. It cannot see the defect this task repairs, because that
// defect is an ABSENT WIRE and not a wrong class: before BRD-34 no production
// code constructed an InterruptController at all, Uart0's controller pointer
// defaulted to nullptr on the assembled machine, and Sim::setInterruptController
// was never called. Every case below therefore drives the ASSEMBLED Board -- at
// the same bus callbacks the core uses -- and observes what reaches the CORE.
//
//   0. THE BOARD PRESENTS NOTHING WHILE ITS CORE HANDLE IS NULL. Uart0's
//      constructor programs its vector into the controller, which presents,
//      and that happens before mcf5307_create has returned.
//
//   1. A TIMER 2 REFERENCE MATCH WITH TMR[ORI] SET AND ICR2 AT MBAR+$04E
//      PROGRAMMED TO 0x84 PRESENTS LEVEL 1, AUTOVECTORED. The whole path is
//      under the assertion: the ICR byte arrives through the MBAR window, the
//      timer raises on the Board's controller, and the controller's winner
//      reaches the core.
//
//   2. CLEARING TER[REF] THROUGH MBAR+$191 DROPS THE PRESENTATION TO LEVEL 0.
//      The board presents on CLEAR as well as on ASSERT, which mcf5307.h's
//      idempotence licenses. A board that presented only on assert would leave
//      the core seeing a level 1 that no longer exists.
//
//   3. THE TIMER AT MBAR+$140 WITH ORI CLEAR PRESENTS NOTHING AT ALL. The
//      assertion is on the CALL COUNT and not on the level, so a board that
//      presented a level 0 unconditionally is red here: a level assertion
//      alone reads the same zero either way. The same case asserts TER1 DID
//      take the match, so the silence is the ORI bit's and not a dead timer's.
//
//   4. ONE CONTROLLER ARBITRATES ACROSS THE WHOLE MACHINE. UART0 (ICR4, level
//      6, vectored 0x42) and timer 2 (ICR2, level 1, autovectored) are pending
//      at once. The UART wins while it is asserted, and when the firmware reads
//      URB the presentation FALLS BACK to the timer's level 1 -- which only a
//      controller that can see BOTH sources can do. A Uart0 handed its own
//      second controller presents a level 0 there, and never a level 6 at all,
//      because the ICR bytes the MBAR window carries reach the Board's
//      controller and not that second one.
//
// WHAT IS NOT HERE, STATED SO THE GREEN IS NOT OVERREAD.
//
//   * The plan's Check line also names a case in which masking the source in
//     IMR at MBAR+$044 suppresses the presentation without disturbing TER.
//     THAT CASE IS NOT IMPLEMENTED AND IT IS NOT SILENTLY DROPPED.
//     InterruptController models three register groups and IMR is not one of
//     them -- interruptController.h names $006 (IRQPAR), $048 (AVR) and
//     $04C..$057 (the internal control block), and its writeRegister ignores
//     every other offset. A mask between a source's pending bit and the
//     arbiter is new arbitration in the CONTROLLER, and BRD-34's own second
//     property forbids exactly that ("THIS TASK ADDS NO ARBITRATION, NO
//     PENDING BIT AND NO PRIORITY DECISION") while its Files: line carries no
//     controller file. The case is therefore a task for whoever adds IMR to
//     BRD-3's class, and it is recorded here rather than faked.
//
//   * It asserts nothing about the boot spin at 0x30055FBC. t1_boot runs
//     firmware on a core the Board does not own and never calls
//     Board::resetMcu, so no interrupt this task wires can reach the firmware
//     under test. Plan section 24.6 row W3-379 measures that and owns it.

#include "../board.h"
#include "../interruptController.h"
#include "../sim.h"
#include "../timer.h"
#include "../uart0.h"

#include <mcf5307.h>

#include <cstdint>
#include <iostream>
#include <string>

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
	// THE RECORDING DOUBLE IS mcf5307_set_irq ITSELF. This target compiles
	// ../board.cpp and links NO mcf5307 archive, so the definition below is
	// the one the Board's present function reaches -- exactly the arrangement
	// t0_sof_tick already uses to observe the Board's call out to isp1181_tick.
	// It is the only arrangement that works: mcf5307.h publishes no getter for
	// the presented interrupt state, and the real symbol cannot be interposed
	// on a link that carries the archive, because `nm -g libmcf5307.a` puts
	// _mcf5307_set_irq in the same member as _takeInterrupt and
	// _pendingInterrupt, which the core's own execution path needs.
	//
	// A CHECK BUILT ON InterruptController::presentedAutovector() WOULD NOT DO.
	// It reads what the CONTROLLER computed, so a present function that
	// hardcoded the autovector argument on its way to the core would leave it
	// green. What is recorded here is what the CORE was handed.
	struct SetIrqRecorder
	{
		int calls = 0;
		mcf5307_ctx* ctx = nullptr;
		int level = -999;
		uint8_t vector = 0xFFu;
		int autovector = -999;

		void reset()
		{
			calls = 0;
			ctx = nullptr;
			level = -999;
			vector = 0xFFu;
			autovector = -999;
		}
	};

	SetIrqRecorder g_recorder;

	// The ICR byte, written out from the MCF5307 User's Manual rather than
	// taken from any header: bit 7 AVEC, IL[2:0] at bits 4:2, IP[1:0] at bits
	// 1:0. This file owns its own copy so the two sides move independently.
	uint8_t makeIcr(const int _level, const int _ip, const bool _avec)
	{
		uint8_t value = uint8_t((_level << 2) | (_ip & 0x03));
		if(_avec)
			value |= 0x80u;
		return value;
	}

	// The TMR bit positions, from MCF5307 UM section 9.4.
	constexpr uint16_t kRst  = 0x0001u;   // bit 0, timer enable
	constexpr uint16_t kFrr  = 0x0008u;   // bit 3, free run / restart
	constexpr uint16_t kOri  = 0x0010u;   // bit 4, output reference interrupt
	constexpr uint16_t kClk1 = 0x0002u;   // CLK[1:0] = 01, the master clock

	constexpr uint8_t kRef = 0x02u;       // TER bit 1, the reference event

	uint16_t makeTmr(const uint16_t _ps, const bool _frr, const bool _ori, const bool _rst)
	{
		uint16_t value = uint16_t(_ps << 8) | kClk1;
		if(_frr) value = uint16_t(value | kFrr);
		if(_ori) value = uint16_t(value | kOri);
		if(_rst) value = uint16_t(value | kRst);
		return value;
	}

	// The MBAR window this fixture uses. The base is this file's own; nothing
	// outside it depends on the number.
	constexpr uint32_t kMbarBase = 0x10000000u;

	// The MBAR-relative offsets, each from the MCF5307 User's Manual.
	constexpr uint32_t kIcrBase = 0x04Cu;   // ICR0, UM Table 8-2
	constexpr uint32_t kTmr1    = 0x140u;
	constexpr uint32_t kTrr1    = 0x144u;
	constexpr uint32_t kTer1    = 0x151u;
	constexpr uint32_t kTmr2    = 0x180u;
	constexpr uint32_t kTrr2    = 0x184u;
	constexpr uint32_t kTer2    = 0x191u;
	constexpr uint32_t kUcr     = 0x1C8u;   // UART0 command register
	constexpr uint32_t kUrb     = 0x1CCu;   // UART0 receiver buffer
	constexpr uint32_t kUimr    = 0x1D4u;   // UART0 interrupt mask register

	// The core's size unit is BYTES, and Board::onRead / Board::onWrite are
	// the pair that takes it.
	constexpr int g_byte = 1;
	constexpr int g_word = 2;

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

	g2::BoardConfig mbarOnlyConfig()
	{
		g2::BoardConfig config;
		config.memory.mbar = {kMbarBase, 0x400u};
		return config;
	}
}

// The mcf5307 and isp1181 entry points ../board.cpp calls. THIS TARGET LINKS
// NO mcf5307 ARCHIVE, and these definitions are why: the recording
// mcf5307_set_irq above is the observation mechanism, and a link that carried
// the archive would refuse it as a duplicate symbol.
//
// Every stub answers the value mcf5307.h defines for a context that can do
// nothing. NOTHING BELOW IS DRIVEN BY ANY CASE IN THIS FILE except
// mcf5307_set_irq: the timers are advanced through Sim::advanceTimers and the
// registers are written through the Board's own bus callbacks, so no case here
// needs a core that executes.
namespace
{
	int g_coreToken = 0;
}

extern "C"
{
	void mcf5307_runtime_init(void)
	{
	}

	mcf5307_ctx* mcf5307_create(void*, mcf5307_read_fn, mcf5307_write_fn,
	                            mcf5307_iack_fn)
	{
		return reinterpret_cast<mcf5307_ctx*>(&g_coreToken);
	}

	void mcf5307_destroy(mcf5307_ctx*)
	{
	}

	uint32_t mcf5307_exec(mcf5307_ctx*, uint32_t)
	{
		return 0u;
	}

	void mcf5307_reset(mcf5307_ctx*, uint32_t, uint32_t)
	{
	}

	uint32_t mcf5307_get_reg(const mcf5307_ctx*, int)
	{
		return 0u;
	}

	int mcf5307_set_reg(mcf5307_ctx*, int, uint32_t)
	{
		return 0;
	}

	int mcf5307_halted(const mcf5307_ctx*)
	{
		return 0;
	}

	int mcf5307_faulted(const mcf5307_ctx*)
	{
		return 0;
	}

	// THE ONE STUB THAT IS THE TEST. Every case below asserts on what arrived
	// here.
	void mcf5307_set_irq(mcf5307_ctx* const ctx, const int level, const uint8_t vector,
	                     const int autovector)
	{
		++g_recorder.calls;
		g_recorder.ctx = ctx;
		g_recorder.level = level;
		g_recorder.vector = vector;
		g_recorder.autovector = autovector;
	}

	isp1181_ctx* isp1181_create(void*, isp1181_irq_fn, isp1181_tx_fn)
	{
		return reinterpret_cast<isp1181_ctx*>(&g_coreToken);
	}

	void isp1181_destroy(isp1181_ctx*)
	{
	}

	void isp1181_tick(isp1181_ctx*, uint32_t)
	{
	}

	uint8_t isp1181_read(isp1181_ctx*, uint32_t)
	{
		return 0u;
	}

	void isp1181_write(isp1181_ctx*, uint32_t, uint8_t)
	{
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 0. THE BOARD PRESENTS NOTHING WHILE ITS CORE HANDLE IS NULL.
	//
	// The controller exists before mcf5307_create returns, and Uart0's own
	// constructor programs its vector into it -- which recomputes and
	// PRESENTS. That presentation has no core to reach. The assertion is on
	// the whole construction, so it is red the moment the guard is removed:
	// without it the sink is called with a null context.
	{
		g_recorder.reset();
		g2::Board board(mbarOnlyConfig());

		checkEqual(g_recorder.calls, 0,
			"A NULL CORE HANDLE PRESENTS NOTHING: constructing a Board called the core zero times");
	}

	// -----------------------------------------------------------------------
	// Case group 1. A TIMER 2 REFERENCE MATCH PRESENTS LEVEL 1, AUTOVECTORED.
	//
	// The ICR byte arrives through the MBAR window, so the read-back below is
	// itself an assertion that the controller -- and not the SIM's plain
	// storage -- answered the address.
	{
		g2::Board board(mbarOnlyConfig());

		mcf5307_bus_status status = MCF5307_BUS_OK;

		const uint32_t icr2 = kMbarBase + kIcrBase + uint32_t(g2::Timer::gTimer2InterruptIndex);
		boardWrite(board, icr2, g_byte, makeIcr(1, 0, true), status);
		checkEqual(boardRead(board, icr2, g_byte, status), uint32_t(0x84u),
			"ICR2 at MBAR+$04E reads back the 0x84 the firmware programs");

		// PS = 0 so one input clock is one tick, FRR set, ORI SET, RST set.
		boardWrite(board, kMbarBase + kTrr2, g_word, 4u, status);
		boardWrite(board, kMbarBase + kTmr2, g_word, makeTmr(0, true, true, true), status);

		g_recorder.reset();
		board.sim().advanceTimers(5);

		checkEqual(boardRead(board, kMbarBase + kTer2, g_byte, status), uint32_t(kRef),
			"the fifth advance sets TER2[REF] through the board");
		checkEqual(g_recorder.calls, 1,
			"THE WIRE EXISTS: the match presented exactly once to the core");
		checkEqual(g_recorder.level, 1,
			"the presentation carries ICR2's level 1");
		checkEqual(g_recorder.autovector, 1,
			"THE AUTOVECTOR IS FORWARDED: ICR2's AVEC bit reaches the core as a non-zero autovector");
		checkEqual(uint32_t(g_recorder.vector), uint32_t(0x00u),
			"timer 2 carries no pass-through vector, so the vector argument is zero");
		check(g_recorder.ctx != nullptr,
			"the presentation reached a non-nil core context");

		// -------------------------------------------------------------------
		// Case group 2. CLEARING TER[REF] DROPS THE PRESENTATION TO LEVEL 0.
		//
		// TER is write-one-to-clear and the firmware's handler writes 2 to
		// MBAR+$191. A board that presented only on ASSERT leaves the core
		// seeing a level 1 that the machine no longer has.
		g_recorder.reset();
		boardWrite(board, kMbarBase + kTer2, g_byte, kRef, status);

		checkEqual(boardRead(board, kMbarBase + kTer2, g_byte, status), uint32_t(0u),
			"the handler's write of 2 to MBAR+$191 clears TER2[REF]");
		checkEqual(g_recorder.calls, 1,
			"THE BOARD PRESENTS ON CLEAR AS WELL AS ON ASSERT: the clear presented once");
		checkEqual(g_recorder.level, 0,
			"the presentation after the clear is MCF5307_IRQ_NONE");
		checkEqual(g_recorder.autovector, 0,
			"a level 0 presentation carries no autovector");
		checkEqual(uint32_t(g_recorder.vector), uint32_t(0x00u),
			"a level 0 presentation carries no vector");
	}

	// -----------------------------------------------------------------------
	// Case group 3. THE TIMER AT MBAR+$140 WITH ORI CLEAR PRESENTS NOTHING.
	//
	// THE ASSERTION IS ON THE CALL COUNT. An assertion on the presented level
	// alone reads the same zero whether the board stayed silent or presented a
	// level 0, so only the count separates the two.
	{
		g2::Board board(mbarOnlyConfig());

		mcf5307_bus_status status = MCF5307_BUS_OK;

		const uint32_t icr1 = kMbarBase + kIcrBase + uint32_t(g2::Timer::gTimer1InterruptIndex);
		boardWrite(board, icr1, g_byte, makeIcr(1, 0, true), status);

		boardWrite(board, kMbarBase + kTrr1, g_word, 4u, status);
		boardWrite(board, kMbarBase + kTmr1, g_word, makeTmr(0, true, false, true), status);

		g_recorder.reset();
		board.sim().advanceTimers(5);

		checkEqual(boardRead(board, kMbarBase + kTer1, g_byte, status), uint32_t(kRef),
			"the timer at MBAR+$140 DID take its reference match");
		checkEqual(g_recorder.calls, 0,
			"ORI CLEAR PRESENTS NOTHING AT ALL: the core was not called");
	}

	// -----------------------------------------------------------------------
	// Case group 4. ONE CONTROLLER ARBITRATES ACROSS THE WHOLE MACHINE.
	//
	// UART0 is ICR4 and carries the vectored 0x42 its own constructor programs
	// into the controller; timer 2 is ICR2 and is autovectored at level 1.
	// BOTH ARE PENDING AT ONCE, so the presentation is an arbitration result
	// and not a relay of whichever source moved last.
	{
		g2::Board board(mbarOnlyConfig());

		mcf5307_bus_status status = MCF5307_BUS_OK;

		const uint32_t icr2 = kMbarBase + kIcrBase + uint32_t(g2::Timer::gTimer2InterruptIndex);
		const uint32_t icr4 = kMbarBase + kIcrBase + uint32_t(g2::Uart0::gUart0InterruptIndex);

		boardWrite(board, icr2, g_byte, makeIcr(1, 0, true), status);
		boardWrite(board, icr4, g_byte, makeIcr(6, 0, false), status);

		boardWrite(board, kMbarBase + kTrr2, g_word, 4u, status);
		boardWrite(board, kMbarBase + kTmr2, g_word, makeTmr(0, true, true, true), status);
		board.sim().advanceTimers(5);

		// The receiver and its RxRDY mask, which is what makes a received
		// character an interrupt condition at all. UM Tables 14-8 and 14-11.
		boardWrite(board, kMbarBase + kUcr, g_byte, 0x01u, status);
		boardWrite(board, kMbarBase + kUimr, g_byte, 0x02u, status);

		g_recorder.reset();
		board.uart0().receive(0x55u);

		checkEqual(g_recorder.calls, 1,
			"UART0's condition presented exactly once");
		checkEqual(g_recorder.level, 6,
			"ONE CONTROLLER SEES BOTH SOURCES: ICR4's level 6 outranks the pending timer's level 1");
		checkEqual(uint32_t(g_recorder.vector), uint32_t(0x42u),
			"the winner is vectored and carries UART0's own 0x42");
		checkEqual(g_recorder.autovector, 0,
			"ICR4's AVEC bit is clear, so the presentation is NOT autovectored");

		g_recorder.reset();
		checkEqual(boardRead(board, kMbarBase + kUrb, g_byte, status), uint32_t(0x55u),
			"reading URB returns the received character");

		checkEqual(g_recorder.calls, 1,
			"emptying the receiver presented exactly once");
		checkEqual(g_recorder.level, 1,
			"IT FALLS BACK TO THE TIMER: only a controller holding BOTH sources can present level 1 here");
		checkEqual(g_recorder.autovector, 1,
			"the fallback carries the timer's AVEC bit and not the UART's");
		checkEqual(uint32_t(g_recorder.vector), uint32_t(0x00u),
			"the fallback carries the timer's empty vector and not the UART's 0x42");
	}

	if(g_failures)
	{
		std::cout << "t0_board_interrupts: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_board_interrupts: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
