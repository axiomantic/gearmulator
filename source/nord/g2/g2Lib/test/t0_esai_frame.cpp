/* t0_esai_frame.cpp -- the check of task SCH-10. Design section 13.10.3.
 *
 * ONE execTX() IS ONE ESAI SLOT, NOT ONE FRAME, and the whole shape of the two
 * calls follows from that. Esai::execTX latches the active slot into the
 * transmit frame, advances the slot counter, and only when that counter passes
 * getTxWordCount() does it resize the frame and fire the transmit callback. So
 * ONE TRANSMIT CALLBACK COSTS getTxWordCount() + 1 SLOTS, and getTxWordCount()
 * is the TDC field of the emulated TCCR -- guest register state, not a
 * constant this project may name.
 *
 * THE TWO CALLS ARE NOT SYMMETRIC, AND THE LIBRARY AND NOT TASTE DECIDES IT.
 * The transmit loop counts FRAMES because a second caller of execTX exists:
 * Esai::writeTransmitControlRegister calls it whenever the guest CHANGES the
 * enabled-transmitter set. So transmit slot phase can be perturbed by exactly
 * one slot at each transmitter enable, and a frame-counted loop re-phases
 * itself on the next quantum. The receive loop has no such caller -- the
 * mirror call is commented out upstream -- so the scheduler is execRX's only
 * caller and a fixed slot count is EXACT.
 *
 * WHAT THE FIXTURE IS, AND WHY IT IS THE LIBRARY'S OWN TYPES.
 *
 * dsp56k::Esai needs an IPeripherals, and both control-register writes reach
 * through it to the DSP. The fixture therefore builds a real Memory, two
 * PeripheralsNop and a real DSP. PeripheralsNop is chosen for a reason and not
 * for convenience: NO EsaiClock IS CONSTRUCTED ANYWHERE IN THIS CHECK.
 * Esai::writeTransmitControlRegister restarts a clock only when the DSP's X
 * peripheral set is a Peripherals56362, and PeripheralsNop is not one, so the
 * branch does not fire. The scheduler drives the ESAI frame and no clock does,
 * which is the whole reason these two functions exist.
 *
 * The transmit and receive callbacks are the fixture's own. The library's
 * default callbacks use ring buffers that BLOCK -- a receive would wait for an
 * input that never arrives -- so a check that left them in place would hang
 * rather than report.
 *
 * WHERE THE DEBUG ASSERTION FITS. Design section 13.10.3 puts
 * assert(slots <= getTxWordCount() + 1) in the implementation, an UPPER bound
 * and not an equality, because a transmitter enable legitimately costs one
 * slot fewer. THE DEFAULT BUILD OF THIS PROJECT IS Release AND DEFINES NDEBUG,
 * so that assertion is not in the shipped translation unit at all. This check
 * therefore asserts the same bound ITSELF, on every quantum, as a test
 * assertion that fails in every build type. An assertion that can only fire in
 * a build nobody runs is not a check.
 */

#include "esaiFrame.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	void checkEqual(const uint64_t observed, const uint64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed %llu, expected %llu\n", what,
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	/* The library's own memory sizes, as its own unit tests use them. */
	dsp56k::DefaultMemoryValidator g_memoryValidator;

	/* THE LIBRARY'S LOG IS SILENCED, AND THE REASON IS STATED.
	 *
	 * The fixture never writes the ESAI transmit data registers, so the
	 * library reports a transmit underrun at every slot it latches. That is
	 * the library behaving correctly against a fixture that supplies no audio,
	 * and it has nothing to do with the slot counts under test -- but it is
	 * one console line for each of about 70,000 slots, which buries the
	 * check's own report and makes the run input-output bound.
	 *
	 * The lines are COUNTED rather than discarded, so "the log was silenced"
	 * stays a statement about volume and not about evidence. */
	uint64_t g_logLines = 0;

	void countLogLine(const std::string&)
	{
		++g_logLines;
	}

	struct Fixture
	{
		dsp56k::Memory           memory;
		dsp56k::PeripheralsNop   peripheralsX;
		dsp56k::PeripheralsNop   peripheralsY;
		dsp56k::DSP              dsp;
		dsp56k::Esai             esai;

		uint64_t transmitCallbacks = 0;
		uint64_t receiveCallbacks  = 0;

		Fixture()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &peripheralsX, &peripheralsY)
			, esai(peripheralsX, dsp56k::MemArea_X)
		{
			Logging::setLogFunc(&countLogLine);

			esai.setWriteTxCallback(
				[this](uint64_t& frameIndex, const dsp56k::Audio::TxFrame&)
				{
					++frameIndex;
					++transmitCallbacks;
				});

			esai.setReadRxCallback(
				[this](uint64_t& frameIndex, dsp56k::Audio::RxFrame& frame)
				{
					++frameIndex;
					++receiveCallbacks;
					frame.resize(dsp56k::Audio::MaxSlotsPerFrame);
				});
		}

		/* TDC and RDC are five-bit fields at a known position, and both bounds
		 * below are DERIVED from the library's own masks. A number written
		 * here would be a second definition of a guest register. */
		void setTransmitWordCount(const uint32_t words)
		{
			esai.writeTransmitClockControlRegister(
				(words << dsp56k::Esai::M_TDC0) & dsp56k::Esai::M_TDC);
		}

		void setReceiveWordCount(const uint32_t words)
		{
			esai.writeReceiveClockControlRegister(
				(words << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);
		}

		void enableTransmitters()
		{
			esai.writeTransmitControlRegister(dsp56k::Esai::M_TEM);
		}

		void disableTransmitters()
		{
			esai.writeTransmitControlRegister(0);
		}

		void enableReceivers()
		{
			esai.writeReceiveControlRegister(dsp56k::Esai::M_REM);
		}

		void disableReceivers()
		{
			esai.writeReceiveControlRegister(0);
		}
	};
}

int main()
{
	/* ---------------- the disabled cases.
	 *
	 * A disabled transmitter is the WHOLE OF THE BOOT PHASE before the kernel
	 * programs TCR, and returning 0 there is the correct behaviour: no
	 * callback fires and no written flag is set. It is also what makes the
	 * transmit loop terminate at all -- Esai::execTX returns without advancing
	 * the slot counter when no transmitter is enabled, so a loop that entered
	 * it could never end. */
	{
		Fixture fixture;

		const uint32_t framesBefore = fixture.esai.getTxFrameCounter();

		checkEqual(g2::transmitDspFrame(fixture.esai), 0u,
			"transmitDspFrame returns 0 when no transmitter is enabled");
		checkEqual(g2::receiveDspFrame(fixture.esai), 0u,
			"receiveDspFrame returns 0 when no receiver is enabled");

		checkEqual(fixture.transmitCallbacks, 0u,
			"a disabled transmit fires no callback");
		checkEqual(fixture.receiveCallbacks, 0u,
			"a disabled receive fires no callback");
		checkEqual(fixture.esai.getTxFrameCounter(), framesBefore,
			"a disabled transmit advances no frame");
	}

	/* ---------------- the transmit slot count, at every word count. */
	{
		for(uint32_t words = 0; words < 8u; ++words)
		{
			Fixture fixture;

			fixture.setTransmitWordCount(words);

			checkEqual(fixture.esai.getTxWordCount(), words,
				"the word count the fixture programmed is the one the ESAI "
				"reports");

			/* THE ENABLE ITSELF COSTS ONE SLOT. Esai's own control-register
			 * write calls execTX once when the enabled set changes, so the
			 * first quantum after an enable is the legitimate case that
			 * returns one slot FEWER. That is exactly why the implementation's
			 * bound is an upper bound and not an equality. */
			fixture.enableTransmitters();

			/* THE BASELINE IS TAKEN AFTER THE ENABLE, and that is a measured
			 * correction and not a tidy-up. At a TDC of 0 a frame is one slot
			 * long, so the single execTX that the enable itself performs
			 * COMPLETES A WHOLE FRAME and fires a callback before this check
			 * has run one quantum. At every larger TDC it does not. A baseline
			 * of zero therefore reports one extra callback at a TDC of 0 and
			 * says nothing about the function under test. */
			const uint64_t callbacksAtEnable = fixture.transmitCallbacks;

			const uint32_t expectedFull = fixture.esai.getTxWordCount() + 1u;

			const uint32_t firstQuantum = g2::transmitDspFrame(fixture.esai);

			check(firstQuantum <= expectedFull,
				"the first quantum after an enable stays inside the upper "
				"bound");

			if(words > 0u)
			{
				checkEqual(firstQuantum, expectedFull - 1u,
					"the quantum after a transmitter enable costs exactly one "
					"slot fewer");
			}

			/* And every quantum after it returns the full count. 1,000
			 * quanta, so a perturbation that re-appeared later would be
			 * caught rather than assumed absent. */
			uint64_t framesSeen = 1u;

			for(uint32_t q = 0; q < 1000u; ++q)
			{
				const uint32_t before = fixture.esai.getTxFrameCounter();
				const uint32_t slots  = g2::transmitDspFrame(fixture.esai);

				++framesSeen;

				if(slots != expectedFull)
				{
					printf("FAIL word count %u, quantum %u: %u slots, not "
						"%u\n", words, q, slots, expectedFull);
					++failures;
					break;
				}

				/* THE UPPER BOUND IS NOT RE-ASSERTED HERE. The equality above
				 * is strictly stronger, so a bound test beside it is a branch
				 * that cannot fire -- which is the shape this project treats
				 * as a defect rather than as harmless. The bound is asserted
				 * where it is the ONLY thing that can be asserted: the first
				 * quantum after an enable, above, and the enable-and-disable
				 * stream at the end of this file. */

				if(fixture.esai.getTxFrameCounter() != before + 1u)
				{
					printf("FAIL word count %u, quantum %u: the frame counter "
						"moved by %u, not by 1\n", words, q,
						fixture.esai.getTxFrameCounter() - before);
					++failures;
					break;
				}
			}

			/* EXACTLY ONE TRANSMIT CALLBACK FOR EACH QUANTUM. The return is
			 * the slot count and the callback is the second observable of the
			 * same event; asserting only one of them would let a body that
			 * counted right and fired twice pass. */
			checkEqual(fixture.transmitCallbacks - callbacksAtEnable, framesSeen,
				"one transmit callback fires for each quantum");
		}
	}

	/* ---------------- the receive slot count is EXACT, at every word count. */
	{
		for(uint32_t words = 0; words < 8u; ++words)
		{
			Fixture fixture;

			fixture.setReceiveWordCount(words);
			fixture.enableReceivers();

			checkEqual(fixture.esai.getRxWordCount(), words,
				"the receive word count the fixture programmed is the one the "
				"ESAI reports");

			const uint32_t expected = fixture.esai.getRxWordCount() + 1u;

			for(uint32_t q = 0; q < 1000u; ++q)
			{
				const uint32_t slots = g2::receiveDspFrame(fixture.esai);

				if(slots != expected)
				{
					printf("FAIL receive word count %u, quantum %u: %u slots, "
						"not %u\n", words, q, slots, expected);
					++failures;
					break;
				}
			}

			/* One receive callback for each quantum. The library reads a new
			 * frame when its slot counter is zero, so a count that was one
			 * too large or one too small would show up here as a callback
			 * count that drifted from the quantum count. */
			checkEqual(fixture.receiveCallbacks, 1000u,
				"one receive callback fires for each quantum");
		}
	}

	/* ---------------- the count follows the GUEST REGISTER and is not a
	 * constant.
	 *
	 * The word count is changed mid-stream and both calls must follow it. A
	 * body that had captured the count once, or that carried a constant, would
	 * pass every case above and fail here. */
	{
		Fixture fixture;

		fixture.setTransmitWordCount(3u);
		fixture.setReceiveWordCount(1u);
		fixture.enableTransmitters();
		fixture.enableReceivers();

		(void) g2::transmitDspFrame(fixture.esai);   /* the re-phasing one */

		checkEqual(g2::transmitDspFrame(fixture.esai), 4u,
			"a transmit frame at a TDC of 3 costs 4 slots");
		checkEqual(g2::receiveDspFrame(fixture.esai), 2u,
			"a receive frame at an RDC of 1 costs 2 slots");

		fixture.setTransmitWordCount(7u);
		fixture.setReceiveWordCount(5u);

		checkEqual(g2::transmitDspFrame(fixture.esai), 8u,
			"the transmit slot count follows a TDC change");
		checkEqual(g2::receiveDspFrame(fixture.esai), 6u,
			"the receive slot count follows an RDC change");
	}

	/* ---------------- the enable-and-disable stream.
	 *
	 * THIS IS WHERE THE UPPER BOUND IS LOAD-BEARING. The guest changes the
	 * enabled-transmitter set between quanta, so the slot phase is perturbed
	 * by exactly one slot at each enable and the exact count for a given
	 * quantum is not predictable from outside. What IS predictable is the
	 * bound the implementation asserts in a debug build:
	 *
	 *     getTxWordCount() <= slots <= getTxWordCount() + 1
	 *
	 * The default build defines NDEBUG and removes that assertion from the
	 * shipped translation unit, so this case is the only place the bound is
	 * really checked in the build this project runs. */
	{
		Fixture fixture;

		fixture.setTransmitWordCount(7u);
		fixture.enableTransmitters();

		const uint32_t full = fixture.esai.getTxWordCount() + 1u;

		uint32_t lowSeen = 0;

		for(uint32_t q = 0; q < 200u; ++q)
		{
			const uint32_t slots = g2::transmitDspFrame(fixture.esai);

			if(slots > full || slots + 1u < full)
			{
				printf("FAIL the enable stream, quantum %u: %u slots is "
					"outside [%u, %u]\n", q, slots, full - 1u, full);
				++failures;
				break;
			}

			if(slots == full - 1u)
				++lowSeen;

			/* Every fifth quantum the guest disables and re-enables, which is
			 * the event that costs one slot. */
			if((q % 5u) == 4u)
			{
				fixture.disableTransmitters();
				fixture.enableTransmitters();
			}
		}

		/* THE PERTURBATION REALLY HAPPENS. Without this the bound would pass
		 * against a stream in which no enable ever cost a slot, and the case
		 * would prove nothing about the reason the bound is an inequality. */
		check(lowSeen > 0u,
			"a transmitter enable really does cost one slot fewer, which is "
			"why the implementation's bound is an inequality");
	}

	/* ---------------- a disable during operation returns to zero. */
	{
		Fixture fixture;

		fixture.setTransmitWordCount(7u);
		fixture.setReceiveWordCount(7u);
		fixture.enableTransmitters();
		fixture.enableReceivers();

		(void) g2::transmitDspFrame(fixture.esai);
		(void) g2::receiveDspFrame(fixture.esai);

		fixture.disableTransmitters();
		fixture.disableReceivers();

		checkEqual(g2::transmitDspFrame(fixture.esai), 0u,
			"a transmitter disabled during operation returns to 0");
		checkEqual(g2::receiveDspFrame(fixture.esai), 0u,
			"a receiver disabled during operation returns to 0");
	}

	if(failures != 0)
	{
		printf("t0_esai_frame: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_esai_frame: all cases passed\n");
	return 0;
}
