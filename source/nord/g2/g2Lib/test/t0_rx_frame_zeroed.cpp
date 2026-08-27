/* t0_rx_frame_zeroed.cpp -- the ESAI receive frame is ZEROED STORAGE, and the
 * registers the G2 producer never writes read a DETERMINISTIC ZERO.
 *
 * WHAT THIS ROW GUARDS, AND WHAT IT DELIBERATELY DOES NOT CLAIM.
 *
 * g2Lib's own frame.h states the precondition verbatim: "EVERY REGISTER INDEX
 * OTHER THAN `reg` IS UNTOUCHED: the conversion writes only [k][reg], so a
 * frame handed to toEsaiFrame must already be zeroed elsewhere." The G2
 * producer writes register 0 alone (chainAdapter.cpp's kAudioReg). NOTHING in
 * g2Lib can satisfy "zeroed elsewhere" on its own -- the storage belongs to
 * dsp56k::Audio::Frame, in the vendored dsp56300 tree. This row is where that
 * precondition is CHECKED rather than assumed, from the consumer that depends
 * on it.
 *
 * THE ZERO MEANS "THE SECOND RECEIVE LINE IS NOT MODELLED". IT DOES NOT MEAN
 * "THE HARDWARE IS SILENT THERE". On a real DSP56303, RX1 ($FFFFA9) is
 * receiver 1's data register and it is fed by the SDI1 pin; the G2 firmware
 * measurably enables RE0 AND RE1 and programs the receive DMA to walk RX0 and
 * RX1 alternately. This emulation models register 0 only -- the second bus is
 * a SEPARATE Esai reached through getEsai1() -- so register 1 carries no
 * modelled signal. A deterministic zero is the correct behaviour of an
 * UNMODELLED input, and the gap between "unmodelled" and "correctly emulated"
 * is real, separate, and larger than this row. NOTHING HERE MAY BE READ AS
 * CLOSING IT.
 *
 * WHY THE STORAGE IS POISONED RATHER THAN TRUSTED TO BE DIRTY. The defect this
 * row exists for is a read of storage that was never written -- and freshly
 * mapped pages read as zero, so the defect is INVISIBLE roughly as often as it
 * is visible. Measured over 20 runs of the real firmware, the count of
 * receive-buffer words carrying garbage ranged over the whole 0..7 of 8 span,
 * three of those runs being fully clean. A row that constructed a frame and
 * hoped for dirt would be a coin flip. Placement-new over a byte pattern this
 * row writes ITSELF makes the pre-fix failure deterministic and makes the
 * post-fix zero mean the constructor produced it.
 */

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/audio.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace
{
	int failures = 0;

	void checkEqual(const uint64_t observed, const uint64_t expected,
		const char* const what)
	{
		if(observed != expected)
		{
			printf("FAIL %s: observed $%06llX, expected $%06llX\n", what,
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected));
			++failures;
		}
	}

	/* THE POISON IS THE ALLOCATE-TIME PATTERN OF THE PLATFORM ALLOCATOR
	 * (MallocPreScribble), and it is chosen for traceability and not for
	 * taste: the same byte is what the real firmware run reports in every
	 * varying word when the allocator is told to scribble, so a failure here
	 * prints the value a reader has already seen in the probe. */
	constexpr unsigned char g_poisonByte = 0xAA;

	uint64_t g_logLines = 0;

	void countLogLine(const std::string&)
	{
		++g_logLines;
	}

	/* The producer's words. Non-zero, distinct per slot, distinct from the
	 * poison, and inside 24 bits -- so a register that reported a neighbouring
	 * slot's word, the poison, or a truncation is a different failure from a
	 * register that reported zero. */
	dsp56k::TWord producerWord(const uint32_t _slot)
	{
		return (0x123456u + _slot * 0x010101u) & 0xFFFFFFu;
	}

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	/* ---------------- ROW A: the storage itself.
	 *
	 * A default-constructed RxFrame over poisoned bytes must read as zero in
	 * EVERY register of EVERY slot -- the complete contents, not a sample of
	 * them. This is the precondition frame.h names, stated as an assertion. */
	void checkFrameConstructsZeroed()
	{
		using RxFrame = dsp56k::Audio::RxFrame;

		alignas(RxFrame) unsigned char storage[sizeof(RxFrame)];

		std::memset(storage, g_poisonByte, sizeof(storage));

		/* NO PARENTHESES, AND THE ROW IS WORTHLESS WITH THEM. `RxFrame()` is
		 * VALUE-initialization, and because Frame's default constructor is
		 * defaulted-on-first-declaration rather than user-provided, value-init
		 * zero-initializes the whole object first -- so the parenthesised form
		 * reads zero over poison EVEN WITH THE DEFECT PRESENT and passes for a
		 * reason that has nothing to do with the member declaration. Observed:
		 * the parenthesised form was written first and passed against the
		 * defect. `new (p) RxFrame` is DEFAULT-initialization, which is what
		 * `RxFrame m_rxFrame;` inside Esai actually does. */
		RxFrame* const frame = new (static_cast<void*>(storage)) RxFrame;

		for(uint32_t slot = 0; slot < dsp56k::Audio::MaxSlotsPerFrame; ++slot)
		{
			for(uint32_t reg = 0; reg < dsp56k::Audio::RxRegisterCount; ++reg)
			{
				char what[128];
				snprintf(what, sizeof(what),
					"a default-constructed RxFrame reads zero at slot %u register %u",
					slot, reg);
				checkEqual((*frame)[slot][reg], 0u, what);
			}
		}

		checkEqual(frame->size(), 0u,
			"a default-constructed RxFrame reports no slots");

		frame->~RxFrame();
	}

	/* ---------------- ROW B: the same property through the real receive path.
	 *
	 * Row A asserts on the container. This row asserts on what the DSP READS,
	 * which is the thing the defect actually corrupted: Esai::readSlotFromFrame
	 * copies the WHOLE RxSlot out of the frame into m_rx, so a register the
	 * producer never wrote reaches Esai::readRX and, from there, $FFFFA9 and
	 * the receive DMA.
	 *
	 * BOTH RECEIVERS ARE ENABLED BECAUSE THE FIRMWARE ENABLES BOTH. readRX
	 * returns 0 unconditionally for a receiver that is not enabled, so a row
	 * that enabled RE0 alone would assert a zero the guard produced and would
	 * pass against the defect. Registers 2 and 3 are deliberately NOT asserted
	 * for that exact reason: RE2/RE3 are not enabled here, their zero would be
	 * the guard's and not the storage's, and an assertion that cannot fail is
	 * worse than none. */
	void checkReceivePathReadsZeroWhereUnmodelled()
	{
		constexpr uint32_t kSlots = 8;

		dsp56k::Memory         memory(g_memoryValidator, 0x080000, 0x800000, 0x200000);
		dsp56k::PeripheralsNop peripheralsX;
		dsp56k::PeripheralsNop peripheralsY;
		dsp56k::DSP            dsp(memory, &peripheralsX, &peripheralsY);

		alignas(dsp56k::Esai) unsigned char storage[sizeof(dsp56k::Esai)];

		std::memset(storage, g_poisonByte, sizeof(storage));

		dsp56k::Esai* const esai = new (static_cast<void*>(storage))
			dsp56k::Esai(peripheralsX, dsp56k::MemArea_X);

		/* The G2 producer, reproduced: it resizes the frame and writes
		 * [k][kAudioReg] and nothing else. This is toEsaiFrameImpl's loop with
		 * reg fixed at 0, which is what chainAdapter.cpp passes. */
		esai->setReadRxCallback(
			[](uint64_t& _frameIndex, dsp56k::Audio::RxFrame& _frame)
			{
				++_frameIndex;

				_frame.resize(kSlots);

				for(uint32_t k = 0; k < kSlots; ++k)
					_frame[k][0] = producerWord(k);
			});

		/* RDC is a five-bit field at a known position and the bound is DERIVED
		 * from the library's own mask; a literal here would be a second
		 * definition of a guest register. RDC holds slots-minus-one, so the
		 * frame wraps after kSlots slots. */
		esai->writeReceiveClockControlRegister(
			((kSlots - 1) << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);

		/* RE0 and RE1, which is rxEnabled=3 -- the value the real firmware
		 * programs and the value the receive DMA's two-word line depends on. */
		esai->writeReceiveControlRegister(
			(1u << dsp56k::Esai::M_RE0) | (1u << dsp56k::Esai::M_RE1));

		checkEqual(esai->hasEnabledReceivers(), 3u,
			"both receivers are enabled, as the firmware programs them");

		for(uint32_t slot = 0; slot < kSlots; ++slot)
		{
			esai->execRX();

			char what[160];

			snprintf(what, sizeof(what),
				"slot %u register 0 carries the word the producer wrote", slot);
			checkEqual(esai->readRX(0), producerWord(slot), what);

			snprintf(what, sizeof(what),
				"slot %u register 1 reads a deterministic zero"
				" (receiver 1 is ENABLED and UNMODELLED, not silent hardware)", slot);
			checkEqual(esai->readRX(1), 0u, what);
		}

		esai->~Esai();
	}
}

int main()
{
	Logging::setLogFunc(&countLogLine);

	checkFrameConstructsZeroed();
	checkReceivePathReadsZeroWhereUnmodelled();

	printf("t0_rx_frame_zeroed: %d failure(s), %llu library log line(s) counted\n",
		failures, static_cast<unsigned long long>(g_logLines));

	return failures == 0 ? 0 : 1;
}
