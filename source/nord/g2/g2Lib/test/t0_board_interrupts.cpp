// The Board owns the interrupt controller and presents to the core.
//
// t0_interrupts already drives InterruptController directly and proves the
// arbitration. It cannot see an absent wire: a Uart0 whose controller pointer
// defaults to nullptr on the assembled machine, or a Sim whose
// setInterruptController is never called. Every case below therefore drives the
// assembled Board -- at the same bus callbacks the core uses -- and observes
// what reaches the core.
//
//   0. The Board presents nothing while its core handle is null. Uart0's
//      constructor programs its vector into the controller, which presents,
//      and that happens before mcf5307_create has returned.
//
//   1. A timer 2 reference match with TMR[ORI] set and ICR2 at MBAR+$04E
//      programmed to 0x84 presents level 1, autovectored. The whole path is
//      under the assertion: the ICR byte arrives through the MBAR window, the
//      timer raises on the Board's controller, and the controller's winner
//      reaches the core.
//
//   2. Clearing TER[REF] through MBAR+$191 drops the presentation to level 0.
//      The board presents on clear as well as on assert, which the core's
//      idempotent set_irq licenses. A board that presented only on assert would
//      leave the core seeing a level 1 that no longer exists.
//
//   3. The timer at MBAR+$140 with ORI clear presents nothing at all. The
//      assertion is on the call count and not on the level, so a board that
//      presented a level 0 unconditionally is red here: a level assertion alone
//      reads the same zero either way. The same case asserts TER1 did take the
//      match, so the silence is the ORI bit's and not a dead timer's.
//
//   4. One controller arbitrates across the whole machine. UART0 (ICR4, level
//      6, vectored 0x42) and timer 2 (ICR2, level 1, autovectored) are pending
//      at once. The UART wins while it is asserted, and when the firmware reads
//      URB the presentation falls back to the timer's level 1 -- which only a
//      controller that can see both sources can do. A Uart0 handed its own
//      second controller presents a level 0 there, and never a level 6 at all,
//      because the ICR bytes the MBAR window carries reach the Board's
//      controller and not that second one.
//
// What is not here:
//
//   * The plan's Check line also names a case in which masking the source in
//     IMR at MBAR+$044 suppresses the presentation without disturbing TER.
//     THAT CASE IS NOT IMPLEMENTED AND IT IS NOT SILENTLY DROPPED.
//     InterruptController models three register groups and IMR is not one of
//     them -- interruptController.h names $006 (IRQPAR), $04B (AVR) and
//     $04C..$057 (the internal control block), and its writeRegister ignores
//     every other offset. A mask between a source's pending bit and the
//     arbiter is new arbitration in the CONTROLLER, and BRD-34's own second
//     property forbids exactly that ("THIS TASK ADDS NO ARBITRATION, NO
//     PENDING BIT AND NO PRIORITY DECISION") while its Files: line carries no
//     controller file. The case is therefore a task for whoever adds IMR to
//     BRD-3's class, and it is recorded here rather than faked.
//
//   * Anything about the boot spin at 0x30055FBC.

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
	// The recording double is mcf5307_set_irq itself. This target compiles
	// ../board.cpp and links no mcf5307 archive, so the definition below is the
	// one the Board's present function reaches. mcf5307.h publishes no getter
	// for the presented interrupt state, and the real symbol cannot be
	// interposed on a link that carries the archive, because
	// `nm -g libmcf5307.a` puts _mcf5307_set_irq in the same member as
	// _takeInterrupt and _pendingInterrupt, which the core's own execution path
	// needs.
	//
	// A check built on InterruptController::presentedAutovector() would not do.
	// It reads what the controller computed, so a present function that
	// hardcoded the autovector argument on its way to the core would leave it
	// green. What is recorded here is what the core was handed.
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

	// The ICR byte: bit 7 AVEC, IL[2:0] at bits 4:2, IP[1:0] at bits 1:0. This
	// file owns its own copy so the two sides move independently.
	uint8_t makeIcr(const int _level, const int _ip, const bool _avec)
	{
		uint8_t value = uint8_t((_level << 2) | (_ip & 0x03));
		if(_avec)
			value |= 0x80u;
		return value;
	}

	// The TMR bit positions.
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

	// The MBAR-relative offsets.
	constexpr uint32_t kIcrBase = 0x04Cu;   // ICR0, UM Table 8-2
	constexpr uint32_t kIrqpar  = 0x006u;   // IRQPAR, UM Table 8-1
	// THE AVR REGISTER BYTE, the base of the longword group that contains it,
	// and one Reserved byte of that group. All three from MCF5307 UM Table
	// B-1, which lists `MBAR+$04B AVCR 8 AUTOVECTOR CONTROL REGISTER` and
	// gives $048, $049 and $04A no row at all. This file owns its own copies
	// so the two sides move independently.
	constexpr uint32_t kAvrRegister      = 0x04Bu;
	constexpr uint32_t kAvrGroupBase     = 0x048u;
	constexpr uint32_t kAvrGroupReserved = 0x049u;
	constexpr uint32_t kTmr1    = 0x140u;
	constexpr uint32_t kTrr1    = 0x144u;
	constexpr uint32_t kTer1    = 0x151u;
	constexpr uint32_t kTmr2    = 0x180u;
	constexpr uint32_t kTrr2    = 0x184u;
	constexpr uint32_t kTer2    = 0x191u;
	constexpr uint32_t kUcr     = 0x1C8u;   // UART0 command register
	constexpr uint32_t kUrb     = 0x1CCu;   // UART0 receiver buffer
	constexpr uint32_t kUimr    = 0x1D4u;   // UART0 interrupt mask register

	// The core's size unit is bytes, and Board::onRead / Board::onWrite are the
	// pair that takes it.
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

// The mcf5307 and isp1181 entry points ../board.cpp calls. This target links no
// mcf5307 archive: the recording mcf5307_set_irq above is the observation
// mechanism, and a link that carried the archive would refuse it as a duplicate
// symbol.
//
// Every stub answers the value mcf5307.h defines for a context that can do
// nothing. Nothing below is driven by any case in this file except
// mcf5307_set_irq: the timers are advanced through Sim::advanceTimers and the
// registers are written through the Board's own bus callbacks, so no case here
// needs a core that executes.
namespace
{
	int g_coreToken = 0;

	// The IRQ callback and the user pointer the Board handed to
	// isp1181_create, captured by the stub below.
	isp1181_irq_fn g_usbIrq = nullptr;
	void* g_usbIrqUser = nullptr;
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

	// The one stub that is the test. Every case below asserts on what arrived
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

	// THE IRQ CALLBACK IS RECORDED AT THE POINT THE BOARD HANDS IT OVER, and
	// case group 5 drives THAT POINTER. A case that called a named Board
	// method instead would stay green with a null callback still installed at
	// isp1181_create -- which is exactly the defect, so the observation has to
	// be taken here and nowhere else.
	isp1181_ctx* isp1181_create(void* const user, const isp1181_irq_fn irq,
	                            isp1181_tx_fn)
	{
		g_usbIrq = irq;
		g_usbIrqUser = user;
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

	/* THE BOARD NOW DRAINS ITS TRANSPORT HUB INTO THE DEVICE ON EVERY QUANTUM
	 * BOUNDARY, so board.cpp references this entry point and a target that
	 * links no mcf5307 archive must supply it. It is a SINK and not a
	 * recorder: nothing in this file drives the hub, so no frame ever reaches
	 * it, and a recorder here would be state no case reads. */
	void isp1181_rx(isp1181_ctx*, int, const uint8_t*, size_t)
	{
	}

	/* THE BOARD MOVES ITS HANDLE OFF THE STUB BACKEND AT CONSTRUCTION, so
	 * board.cpp references this entry point too and a target that links no
	 * mcf5307 archive must supply it.
	 *
	 * IT ANSWERS 1, WHICH IS "THE HANDLE MOVED". The Board reads the return
	 * only to detect a REFUSAL, and a refusal is a state this file's fake
	 * device cannot be in: there is no backend here to refuse. Answering 0
	 * would make every Board in this file print the refusal line, which is
	 * output no case here asks for. */
	int isp1181_set_backend(isp1181_ctx*, int)
	{
		return 1;
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 0. The Board presents nothing while its core handle is null.
	//
	// The controller exists before mcf5307_create returns, and Uart0's own
	// constructor programs its vector into it, which recomputes and presents.
	// That presentation has no core to reach. The assertion is on
	// the whole construction, so it is red the moment the guard is removed:
	// without it the sink is called with a null context.
	{
		g_recorder.reset();
		g2::Board board(mbarOnlyConfig());

		checkEqual(g_recorder.calls, 0,
			"A NULL CORE HANDLE PRESENTS NOTHING: constructing a Board called the core zero times");
	}

	// -----------------------------------------------------------------------
	// Case group 1. A timer 2 reference match presents level 1, autovectored.
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

		// PS = 0 so one input clock is one tick, FRR set, ORI set, RST set.
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
		// Case group 2. Clearing TER[REF] drops the presentation to level 0.
		//
		// TER is write-one-to-clear and the firmware's handler writes 2 to
		// MBAR+$191. A board that presented only on assert leaves the core
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
	// Case group 3. The timer at MBAR+$140 with ORI clear presents nothing.
	//
	// The assertion is on the call count. An assertion on the presented level
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
	// Case group 4. One controller arbitrates across the whole machine.
	//
	// UART0 is ICR4 and carries the vectored 0x42 its own constructor programs
	// into the controller; timer 2 is ICR2 and is autovectored at level 1.
	// Both are pending at once, so the presentation is an arbitration result
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
		// character an interrupt condition at all.
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

	// -----------------------------------------------------------------------
	// Case group 5. THE USB DEVICE'S SERVICE REQUEST REACHES THE CORE AS AN
	// AUTOVECTORED LEVEL 3, AND THE LEVEL IS DERIVED AND NOT WRITTEN DOWN.
	//
	// The level, the autovector bit and the vector number were read out of the
	// G2 firmware, not guessed: BOOT:0x31FE writes its handler to VBR+108 --
	// vector 27, the ColdFire autovector formula 24+level at level 3 -- sets
	// AVR to 0x08 and clears IMR bit 3, and CODE reaches the same three
	// effects through install_autovector(3, 0x30053C38) at its only call site.
	// IRQPAR is never written by either image, so IRQ3 stays at its level 3.
	//
	// WHAT IS DRIVEN IS THE POINTER THE BOARD HANDED TO isp1181_create. A
	// Board that still passes nullptr there records a null callback and this
	// group cannot run at all, which is the red this group was written to
	// produce.
	//
	// THIS GROUP WRITES AVR AT MBAR+$04B, WHERE THE FIRMWARE WRITES IT. It
	// asserts the wire from the device's service request to the core; case
	// group 6 is what pins the register BYTE, and it is the group to read for
	// the manual citation.
	//
	// THE ANTI-HARDCODE ASSERTION IS THE IRQPAR CASE. Level 3 alone is
	// satisfied by a board that writes the constant 3 into the core. Only a
	// board that names the PIN and lets the controller apply UM Table 8-4
	// moves to level 6 when IRQPAR[1] is set, and case 5c asserts exactly
	// that move.
	{
		g2::Board board(mbarOnlyConfig());

		mcf5307_bus_status status = MCF5307_BUS_OK;

		check(g_usbIrq != nullptr,
			"THE IRQ WIRE EXISTS: the Board handed isp1181_create a non-null IRQ callback");
		checkEqual(g_usbIrqUser, static_cast<void*>(&board),
			"the IRQ callback carries THIS Board as its user pointer");

		// The firmware's own AVR write, at MBAR+$04B. Its bit 3 is what makes
		// the level 3 presentation autovectored, and the Board must not
		// supply it.
		boardWrite(board, kMbarBase + kAvrRegister, g_byte, 0x08u, status);
		checkEqual(boardRead(board, kMbarBase + kAvrRegister, g_byte, status), uint32_t(0x08u),
			"AVR reads back the 0x08 the firmware programs");

		if(g_usbIrq != nullptr)
		{
			// 5a. The assert.
			g_recorder.reset();
			g_usbIrq(g_usbIrqUser, 1);

			checkEqual(g_recorder.calls, 1,
				"the device's service request presented exactly once to the core");
			checkEqual(g_recorder.level, 3,
				"IRQ3 AT ITS IRQPAR RESET LEVEL: the presentation carries level 3");
			checkEqual(g_recorder.autovector, 1,
				"AVR BIT 3 REACHES THE CORE: the level 3 presentation is autovectored");
			checkEqual(uint32_t(g_recorder.vector), uint32_t(0x00u),
				"an autovectored external source carries no pass-through vector");

			// 5b. The deassert. A board that presented only on assert would
			// leave the core holding a level 3 the device no longer requests.
			g_recorder.reset();
			g_usbIrq(g_usbIrqUser, 0);

			checkEqual(g_recorder.calls, 1,
				"the deassert presented exactly once");
			checkEqual(g_recorder.level, 0,
				"the deassert drops the presentation to MCF5307_IRQ_NONE");

			// 5c. THE LEVEL IS THE CONTROLLER'S, NOT THE BOARD'S. IRQPAR[1]
			// moves IRQ3 to level 6 by UM Table 8-4. A hardcoded level 3
			// anywhere on this path is red here.
			boardWrite(board, kMbarBase + kIrqpar, g_byte, 0x02u, status);
			g_recorder.reset();
			g_usbIrq(g_usbIrqUser, 1);

			checkEqual(g_recorder.calls, 1,
				"the request under IRQPAR[1] presented exactly once");
			checkEqual(g_recorder.level, 6,
				"THE LEVEL IS DERIVED AND NOT HARDCODED: IRQPAR[1] moves the same pin to level 6");
			checkEqual(g_recorder.autovector, 0,
				"AVR bit 6 is clear, so the level 6 presentation is NOT autovectored");
		}
	}

	// -----------------------------------------------------------------------
	// Case group 6. THE AVR BYTE THE FIRMWARE ACTUALLY WRITES, AT MBAR+$04B.
	//
	// WHY THIS GROUP WRITES $04B AND NOT $048. A test that writes where the
	// MODEL listens cannot see a model listening at the wrong byte; only a
	// test that writes where the FIRMWARE writes can.
	// MCF5307 UM Table B-1 lists the register by address and width:
	// `MBAR+$04B AVCR 8 AUTOVECTOR CONTROL REGISTER $00 R/W`. There is no row
	// for $048, $049 or $04A -- Table 8-1 gives the $048 row four byte columns
	// and names the first three Reserved -- so $048 is the group base and $04B
	// is the register byte. Both firmware images agree: BOOT:0x320E and
	// CODE:0x3005827E / CODE:0x30058522 each load $1000004B into a0 and touch
	// the byte there, and no ALIGNED reference to $10000048 exists in either
	// image. This group writes where the FIRMWARE writes.
	{
		g2::Board board(mbarOnlyConfig());

		mcf5307_bus_status status = MCF5307_BUS_OK;

		// 6a. KNOWN POSITIVE. The router owns $04B, and the way that is
		// visible from the bus is the interrupt block's byte-only rule: a
		// wider access to an owned offset is refused.
		boardWrite(board, kMbarBase + kAvrRegister, g_word, 0x0008u, status);
		checkEqual(int(status), int(MCF5307_BUS_SIZE_ILLEGAL),
			"KNOWN POSITIVE: a word write to $04B is refused, so the interrupt block owns it");

		boardRead(board, kMbarBase + kAvrRegister, g_word, status);
		checkEqual(int(status), int(MCF5307_BUS_SIZE_ILLEGAL),
			"KNOWN POSITIVE: a word read of $04B is refused, so the interrupt block owns it");

		// 6b. KNOWN NEGATIVE, SAME PREDICATE. $049 is a Reserved byte of the
		// SAME longword group. isInterruptOwned is the one predicate that
		// resolves both, and it must answer NO here, so the identical word
		// access falls through to the SIM and is accepted. A predicate that
		// swallowed the whole $048..$04B group would be red on this line.
		boardWrite(board, kMbarBase + kAvrGroupReserved, g_word, 0x0008u, status);
		checkEqual(int(status), int(MCF5307_BUS_OK),
			"KNOWN NEGATIVE: a word write to the Reserved $049 is accepted, so the interrupt block does NOT own it");

		boardRead(board, kMbarBase + kAvrGroupReserved, g_word, status);
		checkEqual(int(status), int(MCF5307_BUS_OK),
			"KNOWN NEGATIVE: a word read of the Reserved $049 is accepted, so the interrupt block does NOT own it");

		// 6c. THE FIRMWARE'S OWN WRITE, ASSERTED FROM THE CONTROLLER'S STATE.
		// boardRead alone would be satisfied by the SIM's flat backing store
		// answering the byte it was handed, which is exactly what happened
		// before this repair. readRegister is the CONTROLLER, so only a byte
		// that actually reached the controller reads back here.
		boardWrite(board, kMbarBase + kAvrRegister, g_byte, 0x08u, status);
		checkEqual(int(status), int(MCF5307_BUS_OK),
			"the firmware's byte write to $04B is accepted");
		checkEqual(uint32_t(board.interrupts().readRegister(kAvrRegister)), uint32_t(0x08u),
			"THE BYTE REACHES THE CONTROLLER: AVR reads back 0x08 from the controller itself");
		checkEqual(boardRead(board, kMbarBase + kAvrRegister, g_byte, status), uint32_t(0x08u),
			"and the same byte reads back through the MBAR window");

		// 6d. END TO END. Nothing below writes $048. The AVR bit 3 programmed
		// at $04B above is the only thing that can make this autovectored.
		check(g_usbIrq != nullptr,
			"the IRQ wire exists for the end-to-end autovector case");

		if(g_usbIrq != nullptr)
		{
			g_recorder.reset();
			g_usbIrq(g_usbIrqUser, 1);

			checkEqual(g_recorder.calls, 1,
				"the device's service request presented exactly once to the core");
			checkEqual(g_recorder.level, 3,
				"the presentation carries IRQ3's level 3");

			// ASSERTED FROM THE CONTROLLER'S STATE, NOT FROM THE WRITE. These
			// two read what the arbiter COMPUTED. A board that forwarded a
			// hardcoded autovector to the core would be green on the recorder
			// and red here.
			checkEqual(board.interrupts().presentedLevel(), 3,
				"CONTROLLER STATE: the arbiter's own winner is level 3");
			checkEqual(board.interrupts().presentedAutovector(), 1,
				"CONTROLLER STATE: THE AVR WRITE AT $04B REACHED THE ARBITER, so level 3 is autovectored");

			checkEqual(g_recorder.autovector, 1,
				"and that autovector bit is what the CORE was handed");
			checkEqual(uint32_t(g_recorder.vector), uint32_t(0x00u),
				"an autovectored external source carries no pass-through vector");
		}

		// 6e. $048 IS NOT THE REGISTER. Table B-1 gives it no row, so a byte
		// written there must NOT reach the controller. Without this line the
		// repair could be a widening that keeps the old wrong offset alive.
		g2::Board second(mbarOnlyConfig());
		boardWrite(second, kMbarBase + kAvrGroupBase, g_byte, 0x08u, status);
		checkEqual(uint32_t(second.interrupts().readRegister(kAvrRegister)), uint32_t(0x00u),
			"$048 IS RESERVED: a byte written to the group base does not reach AVR");

		if(g_usbIrq != nullptr)
		{
			g_recorder.reset();
			g_usbIrq(g_usbIrqUser, 1);

			checkEqual(g_recorder.calls, 1,
				"the request on the second board presented exactly once");
			checkEqual(second.interrupts().presentedLevel(), 3,
				"CONTROLLER STATE: the second board's arbiter presents level 3");
			checkEqual(second.interrupts().presentedAutovector(), 0,
				"CONTROLLER STATE: a $048 write leaves AVR clear, so level 3 is NOT autovectored");
		}
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
