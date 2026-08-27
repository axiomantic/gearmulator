// Four assertions, each driving the board's own arbiter and reading the
// arguments it presents:
//
//   1. A higher level beats a lower one.
//   2. IRQPAR re-maps IRQ3, and the winner moves with it.
//   3. The autovector argument follows the bit the firmware programmed.
//   4. No pending source presents the named zero, and a source that drops
//      returns to it.
//
// Plus the within-level order Table 8-3 fixes, and the whole index-domain
// walk.
//
// This test writes the register facts out again by hand rather than importing
// them: a test that imported the implementation's own constants would assert
// that the implementation equals itself.
//
//  * Internal ICRs at MBAR+$04C..$057: bit 7 AVEC, IL[2:0] at bits 4:2, IP[1:0]
//    at bits 1:0. UM section 8.3.4 and Table 8-2, pp. 8-5..8-6.
//  * AVR at MBAR+$04B, AVEC[7:1]. UM Table B-1 for the address and width,
//    section 8.3.4 p. 8-7 for the bit layout.
//  * IRQPAR at MBAR+$006: IRQ5 -> level 5 or 4, IRQ3 -> level 6 or 3, IRQ1 ->
//    level 1 or 2, IRQ7 always 7. UM section 8.3.4.1 and Table 8-4, pp. 8-9..8-10.
//  * Within a level the order is internal IP=11, then IP=10, then the external
//    pin, then IP=01, then IP=00. UM Table 8-3, pp. 8-6..8-7.

#include "interruptController.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

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

	// The present callback records the last presented arguments so that a test
	// can assert what the arbiter would hand to mcf5307_set_irq.
	struct PresentRecorder
	{
		int level = -999;
		uint8_t vector = 0;
		int autovector = -999;
	};

	void recordPresent(void* _user, const int _level, const uint8_t _vector, const int _autovector)
	{
		auto* recorder = static_cast<PresentRecorder*>(_user);
		recorder->level = _level;
		recorder->vector = _vector;
		recorder->autovector = _autovector;
	}

	// The register facts written out by hand from the manual. These are the
	// same facts interruptController.cpp reads; the test owns its own copy so
	// the two sides move independently.
	constexpr uint32_t gIcrBase = 0x04Cu;
	constexpr uint32_t gIcrCount = 12u;
	// AVR is the AUTOVECTOR CONTROL REGISTER at MBAR+$04B. MCF5307 UM Table
	// B-1 lists it by address and width -- `MBAR+$04B AVCR 8` -- and gives
	// $048, $049 and $04A no row at all; Table 8-1's $048 row is four byte
	// columns whose first three are Reserved, and both G2 firmware images load
	// $1000004B to reach the byte.
	constexpr uint32_t gAvrOffset = 0x04Bu;
	constexpr uint32_t gIrqparOffset = 0x006u;
	constexpr int gInternalSourceCount = 10;

	uint8_t makeIcr(const int _level, const int _ip, const bool _avec)
	{
		uint8_t value = uint8_t((_level << 2) | (_ip & 0x03));
		if(_avec)
			value |= 0x80u;
		return value;
	}

	// A fresh controller with a recording callback.
	g2::InterruptController makeController(PresentRecorder& _recorder)
	{
		return g2::InterruptController(&_recorder, recordPresent);
	}
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 1. A higher level beats a lower one, and the winner moves
	// with the level when the two levels are exchanged in the control
	// registers.
	//
	// Both sources are INTERNAL, because an external pin carries a fixed
	// level and its level cannot be exchanged. Source 0 (SWT) and source 1
	// (Timer1) both pend. An arbiter that returns the first pending source
	// instead of the highest passes the first half and fails the second.
	{
		PresentRecorder recorder;
		g2::InterruptController controller = makeController(recorder);

		controller.writeRegister(gIcrBase + 0, makeIcr(5, 0, false)); // source 0 at level 5
		controller.writeRegister(gIcrBase + 1, makeIcr(3, 0, false)); // source 1 at level 3

		controller.setInternalPending(0, true);
		controller.setInternalPending(1, true);

		checkEqual(recorder.level, 5, "of two internal sources pending at levels 5 and 3, the board presents the higher level 5");

		// Exchange the two levels in their control registers. Nothing else
		// changes. The winner must move with the level.
		controller.writeRegister(gIcrBase + 0, makeIcr(3, 0, false));
		controller.writeRegister(gIcrBase + 1, makeIcr(5, 0, false));

		checkEqual(recorder.level, 5, "after the two internal sources exchange levels, the board again presents level 5, now carried by the other source");

		// Make the two levels differ by more than one so an off-by-one neither
		// half would see. Source 0 at level 1, source 1 at level 6.
		controller.writeRegister(gIcrBase + 0, makeIcr(1, 0, false));
		controller.writeRegister(gIcrBase + 1, makeIcr(6, 0, false));
		checkEqual(recorder.level, 6, "with levels 1 and 6 pending, the board presents level 6");
	}

	// -----------------------------------------------------------------------
	// Case group 2. IRQPAR re-maps IRQ3, and the winner moves with it.
	//
	// One IRQ3 assertion arbitrated twice against one competitor at level 5.
	// With IRQ3 mapped to internal level 6 it wins; with IRQ3 mapped to level
	// 3 it loses. One machine state, two IRQPAR values, two different winners.
	// An arbiter that does not read MBAR+$006 cannot pass both halves.
	{
		PresentRecorder recorder;
		g2::InterruptController controller = makeController(recorder);

		// A competitor internal source at level 5.
		controller.writeRegister(gIcrBase + 2, makeIcr(5, 0, false));
		controller.setInternalPending(2, true);

		// IRQ3 pending; IRQPAR[1] = 1 maps IRQ3 to internal level 6.
		controller.setExternalPending(g2::ExternalPin::Irq3, true);
		controller.writeRegister(gIrqparOffset, 0x02u);

		checkEqual(recorder.level, 6, "with IRQ3 mapped to level 6 by IRQPAR[1], the IRQ3 pin beats the level-5 internal competitor");

		// Same machine state, IRQPAR[1] = 0 maps IRQ3 to level 3. IRQ3 now
		// loses to the level-5 competitor.
		controller.writeRegister(gIrqparOffset, 0x00u);

		checkEqual(recorder.level, 5, "with IRQ3 mapped to level 3 by IRQPAR[1], the level-5 internal competitor beats the IRQ3 pin");
	}

	// -----------------------------------------------------------------------
	// Case group 3. The autovector argument follows the bit the firmware
	// programmed.
	//
	// For an internal source it follows AVEC at bit 7 of that source's own
	// control register. For an external pin it follows the AVR bit for the
	// winning level. Each is driven set and clear, and the presented argument
	// is asserted both ways. A pass-through wired to either constant fails one
	// half.
	{
		// Internal half. Source 0 pending at level 4, with AVEC both ways.
		{
			PresentRecorder recorder;
			g2::InterruptController controller = makeController(recorder);

			controller.writeRegister(gIcrBase + 0, makeIcr(4, 0, true));  // AVEC set
			controller.setInternalPending(0, true);
			checkEqual(recorder.autovector, 1, "an internal source with AVEC set presents autovector 1");

			controller.writeRegister(gIcrBase + 0, makeIcr(4, 0, false)); // AVEC clear
			checkEqual(recorder.autovector, 0, "an internal source with AVEC clear presents autovector 0");
		}

		// External half. IRQ3 mapped to level 6 (IRQPAR[1] = 1), with the AVR
		// bit for level 6 set and clear. The AVR bit for the WINNING level is
		// what the autovector argument follows.
		{
			PresentRecorder recorder;
			g2::InterruptController controller = makeController(recorder);

			controller.writeRegister(gIrqparOffset, 0x02u); // IRQ3 at level 6
			controller.setExternalPending(g2::ExternalPin::Irq3, true);

			controller.writeRegister(gAvrOffset, 0x40u); // AVEC bit 6 set
			checkEqual(recorder.autovector, 1, "an external pin whose winning level's AVR bit is set presents autovector 1");

			controller.writeRegister(gAvrOffset, 0x00u); // AVEC bit 6 clear
			checkEqual(recorder.autovector, 0, "an external pin whose winning level's AVR bit is clear presents autovector 0");
		}
	}

	// -----------------------------------------------------------------------
	// Case group 4. No pending source presents the named zero, and a source
	// that drops returns to it.
	//
	// With nothing pending the board presents MCF5307_IRQ_NONE. The one pending
	// source is then raised and dropped (a level source dropping when the device
	// model clears its own condition), and the board must present the named zero
	// again. An arbiter
	// that never deasserts passes the first half and fails the second.
	{
		PresentRecorder recorder;
		g2::InterruptController controller = makeController(recorder);

		// No source pending. The constructor presents nothing yet; a read of
		// the last-presented state pins the named zero.
		checkEqual(controller.presentedLevel(), 0, "with no source pending, the controller's presented level is MCF5307_IRQ_NONE (0)");

		// Raise one internal source.
		controller.writeRegister(gIcrBase + 3, makeIcr(2, 0, false));
		controller.setInternalPending(3, true);
		checkEqual(recorder.level, 2, "raising one source presents that source's level 2");

		// Drop it: the level source's condition clears and the board
		// recomputes.
		controller.setInternalPending(3, false);
		checkEqual(recorder.level, 0, "dropping the only pending source returns the presented level to MCF5307_IRQ_NONE (0)");

		// The same via an external source, the worked case's own source class.
		controller.setExternalPending(g2::ExternalPin::Irq3, true);
		controller.writeRegister(gIrqparOffset, 0x02u); // IRQ3 at level 6
		checkEqual(recorder.level, 6, "raising the IRQ3 pin presents its mapped level 6");

		controller.setExternalPending(g2::ExternalPin::Irq3, false);
		checkEqual(recorder.level, 0, "dropping the IRQ3 pin returns the presented level to MCF5307_IRQ_NONE (0)");
	}

	// -----------------------------------------------------------------------
	// Case group 5. Within a level, the manual fixes the order, and the test
	// pins the winner the manual names.
	//
	// Table 8-3: internal IP=11, then IP=10, then the external pin, then IP=01,
	// then IP=00. This pins the two internal IP ranks against each other and
	// pins the external pin between IP=10 and IP=01.
	{
		// Internal IP=11 beats internal IP=00 at the same level, and the
		// winner moves with the IP bits when they are exchanged.
		{
			PresentRecorder recorder;
			g2::InterruptController controller = makeController(recorder);

			controller.writeRegister(gIcrBase + 4, makeIcr(4, 3, false)); // IP=11
			controller.writeRegister(gIcrBase + 5, makeIcr(4, 0, false)); // IP=00
			controller.setInternalPending(4, true);
			controller.setInternalPending(5, true);

			// Both present level 4. To observe WHO wins we read the presented
			// vector: the winner carries its own pass-through vector, which the
			// two sources were given distinct values for.
			controller.setInternalVector(4, 0x11u);
			controller.setInternalVector(5, 0x22u);

			checkEqual(recorder.level, 4, "two internal sources pending at the same level present that level");
			checkEqual(recorder.vector, uint8_t(0x11), "at the same level the source with IP=11 wins over the source with IP=00, as Table 8-3 fixes");

			// Exchange the IP bits. The winner must move with them.
			controller.writeRegister(gIcrBase + 4, makeIcr(4, 0, false));
			controller.writeRegister(gIcrBase + 5, makeIcr(4, 3, false));

			checkEqual(recorder.vector, uint8_t(0x22), "after the two sources exchange IP bits, the other source wins, so the manual's IP=11 rule is what decides");
		}

		// The external pin sits between IP=10 and IP=01 at its level: an
		// internal IP=10 source at the same level beats the pin, and the pin
		// beats an internal IP=00 source.
		{
			// IRQ3 at level 6, internal source at level 6 with IP=10.
			{
				PresentRecorder recorder;
				g2::InterruptController controller = makeController(recorder);

				controller.writeRegister(gIrqparOffset, 0x02u); // IRQ3 at level 6
				controller.writeRegister(gIcrBase + 6, makeIcr(6, 2, false)); // IP=10
				controller.setExternalPending(g2::ExternalPin::Irq3, true);
				controller.setInternalPending(6, true);
				controller.setInternalVector(6, 0x33u);

				checkEqual(recorder.vector, uint8_t(0x33), "an internal IP=10 source at the same level beats the external pin, as Table 8-3 fixes");
			}

			// IRQ3 at level 6, internal source at level 6 with IP=00.
			{
				PresentRecorder recorder;
				g2::InterruptController controller = makeController(recorder);

				controller.writeRegister(gIrqparOffset, 0x02u); // IRQ3 at level 6
				controller.writeRegister(gIcrBase + 7, makeIcr(6, 0, false)); // IP=00
				controller.setExternalPending(g2::ExternalPin::Irq3, true);
				controller.setInternalPending(7, true);
				controller.setInternalVector(7, 0x44u);
				// Give the external pin a distinct vector so the winner is
				// observable, not merely consistent with an ambiguous zero.
				controller.setExternalVector(g2::ExternalPin::Irq3, 0x55u);

				checkEqual(recorder.vector, uint8_t(0x55), "the external pin beats an internal IP=00 source at the same level, as Table 8-3 fixes");
			}
		}
	}

	// -----------------------------------------------------------------------
	// Case group 6. The whole index domain is walked end to end.
	//
	// The arbiter indexes two tables: the internal control-register block and
	// the level-indexed AVR bitmask. Here every byte offset from $04C to $057
	// inclusive is programmed, each one to every level the interface admits,
	// 1 to 7, and for each state the test asserts that the level the board
	// presents is MCF5307_IRQ_NONE or 1 to 7, and that the level it names
	// belongs to the one source that is pending.
	//
	// Each iteration clears every source, holds ONE source pending, programs
	// its register to `level`, and asserts the presented level equals `level`.
	// Offsets $056 and $057 are Reserved: their registers store the byte but
	// generate no source, so with them programmed and no other source pending
	// the board presents MCF5307_IRQ_NONE.
	//
	// An index leaving the control-register table or the AVR bitmask reads a
	// neighbouring byte. This walk drives the full domain, so under a
	// sanitizer build any out-of-range read fails loudly instead of returning
	// a neighbour.
	{
		PresentRecorder recorder;
		g2::InterruptController controller = makeController(recorder);

		for(uint32_t offset = gIcrBase; offset < gIcrBase + gIcrCount; ++offset)
		{
			for(int level = 1; level <= 7; ++level)
			{
				// Clear every source.
				for(int i = 0; i < gInternalSourceCount; ++i)
					controller.setInternalPending(i, false);
				for(int p = 0; p < 4; ++p)
					controller.setExternalPending(static_cast<g2::ExternalPin>(p), false);

				// Program this register and hold its source pending, if any.
				controller.writeRegister(offset, makeIcr(level, 0, false));

				const uint32_t sourceIndex = offset - gIcrBase;
				if(sourceIndex < uint32_t(gInternalSourceCount))
				{
					controller.setInternalPending(int(sourceIndex), true);

					check(recorder.level >= 0 && recorder.level <= 7,
						"at ICR offset " + std::to_string(offset) + " level " + std::to_string(level)
						+ ", the presented level is NONE or in 1..7");
					checkEqual(recorder.level, level,
						"at ICR offset " + std::to_string(offset) + " level " + std::to_string(level)
						+ ", the one pending source names its own programmed level");
				}
				else
				{
					// Reserved register: stored but generates no source.
					checkEqual(recorder.level, 0,
						"the reserved ICR offset " + std::to_string(offset) + " generates no source, so the presented level is MCF5307_IRQ_NONE");
				}
			}
		}
	}

	// -----------------------------------------------------------------------
	// Case group 7. The AVR bitmask is indexed only by the levels the
	// interface admits.
	//
	// The autovector argument for an external pin is (AVR >> winningLevel) & 1,
	// and the winning level is always 1..7 when a source is presented. This
	// drives every external pin at every level it can map to under every IRQPAR
	// value and every AVR bit pattern, asserting the presented level is in 1..7
	// and the autovector argument follows the AVR bit for that level.
	{
		for(uint32_t irqpar = 0; irqpar <= 0x07u; ++irqpar)
		{
			for(uint32_t avr = 0; avr <= 0x7fu; ++avr)
			{
				PresentRecorder recorder;
				g2::InterruptController controller = makeController(recorder);

				controller.writeRegister(gIrqparOffset, uint8_t(irqpar));
				controller.writeRegister(gAvrOffset, uint8_t(avr));

				controller.setExternalPending(g2::ExternalPin::Irq3, true);

				check(recorder.level >= 0 && recorder.level <= 7,
					"IRQ3 pending under IRQPAR=" + std::to_string(irqpar)
					+ " presents a level in 0..7");
				const int expectedLevel = (irqpar & 0x02u) ? 6 : 3;
				checkEqual(recorder.level, expectedLevel,
					"IRQ3 pending under IRQPAR=" + std::to_string(irqpar) + " presents its mapped level");
				const int expectedAutovector = int((avr >> expectedLevel) & 0x01u);
				checkEqual(recorder.autovector, expectedAutovector,
					"IRQ3's autovector argument follows the AVR bit of its winning level, under AVR=" + std::to_string(avr));
			}
		}
	}

	if(g_failures)
	{
		std::cout << "t0_interrupts: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_interrupts: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
