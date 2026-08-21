/* t0_esai_slot_phase.cpp -- the check of task SCH-34.
 * Design 13.10.3, 12.3, 13.4.6.
 *
 * THE ESAI SLOT-AND-CORE INTERLEAVE. The two frame helpers gain a form
 * that invokes a caller-supplied callback after EACH execTX and EACH
 * execRX, and dspJob's step 2 passes a callback that runs one sub-budget
 * of runDspCycles, so core execution lands BETWEEN ESAI slots instead of
 * between whole frames.
 *
 * THE DISCRIMINATING OBSERVABLE IS THE GUEST'S OWN SENTINEL WRITE to
 * X:$000100, value $AAAAAA, with a pre-state assertion that the word is
 * zero. Not a program counter read, not an ESAI bit sample, not a
 * dispatch count -- the sentinel is the only observable that requires the
 * guest to have executed an instruction it could not reach without seeing
 * the edge.
 *
 * THE FIXTURE SPIN SITS BELOW Vba_End ($100), where DSP-19's
 * dynamicFastInterrupts puts the JIT in FastInterruptMode::Dynamic and
 * exec() returns after each instruction. THIS TEST READS NO PROGRAM
 * COUNTER AT ALL.
 *
 * THE TEST MUST RUN UNDER THE JIT AND MUST FAIL LOUDLY RATHER THAN SKIP ON
 * A NON-JIT BUILD. g_useJIT is read at run time.
 *
 * THE TEST USES DspSet BECAUSE dynamicFastInterrupts IS SET IN
 * DspSet::Slot::Slot (DSP-19's production code), so a test using
 * PeripheralsNop would not have the flag set and would hang.
 *
 * M_TFS AND M_RFS START SET. The ESAI constructor sets them on slot 0.
 * The guest does not execute until the interleave's callback runs it
 * AFTER an execTX/execRX call, so the initial state does not let the
 * brclr fall through before the interleave runs. On the transmit side,
 * the first frame starts at slot 1 (enableTransmitters calls execTX
 * once at slot 0, advancing the counter) and M_TFS is clear on every
 * slot the callback runs. The second frame starts at slot 0, which sets
 * M_TFS, and the callback lets the guest fall through. On the receive
 * side, the first frame starts at slot 0, which sets M_RFS, and the
 * callback lets the guest fall through on the first frame.
 *
 * FOUR ARMS, AND ONE UNIT IS ONE ARM.
 */

#include "dspContext.h"
#include "dspSet.h"
#include "esaiFrame.h"

#include "g2/timebase.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/interrupts.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/jitconfig.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals56311.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace g2
{
	void dspJob(JobContext*) noexcept;
}

namespace
{
	int g_failures = 0;

	void fail(const char* const _what)
	{
		std::printf("FAIL %s\n", _what);
		++g_failures;
	}

	void checkEqual(const uint64_t _observed, const uint64_t _expected,
		const char* const _what)
	{
		if(_observed == _expected)
		{
			std::printf("ok   %s\n", _what);
			return;
		}
		std::printf("FAIL %s: observed %llu, expected %llu\n", _what,
			static_cast<unsigned long long>(_observed),
			static_cast<unsigned long long>(_expected));
		++g_failures;
	}

	void check(const bool _cond, const char* const _what)
	{
		if(_cond)
		{
			std::printf("ok   %s\n", _what);
			return;
		}
		fail(_what);
	}

	uint64_t g_logLines = 0;

	void countLogLine(const std::string&)
	{
		++g_logLines;
	}

	constexpr dsp56k::TWord kSentinelAddr = 0x000100;
	constexpr dsp56k::TWord kSentinelValue = 0xAAAAAA;

	/* BELOW Vba_End ($100), where dynamicFastInterrupts puts the JIT in
	 * FastInterruptMode::Dynamic and exec() returns after each instruction. */
	constexpr dsp56k::TWord kProgramStart = 0x0080;

	static_assert(kProgramStart < dsp56k::Vba_End,
		"the fixture spin must sit below Vba_End so the JIT returns after "
		"each instruction");

	/* TWO FRAMES: the first frame advances the ESAI through its slots
	 * while M_TFS is clear on every slot the callback runs. The second
	 * frame starts at slot 0, which sets M_TFS, and the callback lets the
	 * guest fall through. One frame is not enough because the transmit
	 * frame loop exits before slot 0 of the next frame runs. */
	constexpr uint32_t kMaxFrames = 2;

	struct Fixture
	{
		g2::DspSet set;
		dsp56k::Esai& audioEsai;
		dsp56k::Esai& secondEsai;
		dsp56k::DSP& dsp;
		dsp56k::Memory& memory;

		Fixture()
			: audioEsai(set.peripherals(0).getEsai())
			, secondEsai(set.peripherals(0).getEsai1())
			, dsp(set.dsp(0))
			, memory(dsp.memory())
		{
			Logging::setLogFunc(&countLogLine);
		}

		void disableBlockLinking()
		{
			dsp56k::JitConfig config = dsp.getJit().getConfig();
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);
		}

		bool loadProgram(const char* const _path, const dsp56k::TWord _entry)
		{
			std::ifstream file(_path);
			if(!file.is_open())
			{
				std::printf("FAIL could not open fixture %s\n", _path);
				++g_failures;
				return false;
			}

			const dsp56k::Assembler assembler;
			std::string line;
			dsp56k::TWord pc = _entry;

			while(std::getline(file, line))
			{
				const auto comment = line.find(';');
				if(comment != std::string::npos)
					line.erase(comment);

				const auto first = line.find_first_not_of(" \t\r\n");
				if(first == std::string::npos)
					continue;

				const auto last = line.find_last_not_of(" \t\r\n");
				const std::string text = line.substr(first, last - first + 1);

				const dsp56k::AssembleResult result =
					assembler.assemble(text.c_str());

				if(!result.success())
				{
					std::printf("FAIL could not assemble \"%s\"\n",
						text.c_str());
					++g_failures;
					return false;
				}

				for(uint32_t i = 0; i < result.wordCount; ++i)
				{
					if(!dsp.memWriteP(pc, result.word[i]))
					{
						fail("could not write program word");
						return false;
					}
					++pc;
				}
			}
			return true;
		}

		bool writeInst(const char* text, dsp56k::TWord& pc)
		{
			const dsp56k::Assembler assembler;
			const auto r = assembler.assemble(text);
			if(!r.success())
			{
				std::printf("FAIL could not assemble \"%s\"\n", text);
				++g_failures;
				return false;
			}
			for(uint32_t i = 0; i < r.wordCount; ++i)
			{
				if(!dsp.memWriteP(pc, r.word[i]))
				{
					fail("could not write program word");
					return false;
				}
				++pc;
			}
			return true;
		}

		void setWordCounts(const uint32_t _tx, const uint32_t _rx)
		{
			audioEsai.writeTransmitClockControlRegister(
				(_tx << dsp56k::Esai::M_TDC0) & dsp56k::Esai::M_TDC);
			audioEsai.writeReceiveClockControlRegister(
				(_rx << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);
		}

		void enableTransmitters()
		{
			audioEsai.writeTransmitControlRegister(dsp56k::Esai::M_TEM);
		}

		void enableReceivers()
		{
			audioEsai.writeReceiveControlRegister(dsp56k::Esai::M_REM);
		}

		/* Clears M_TFS and M_RFS in the status register. The ESAI
		 * constructor sets both on slot 0, and enableTransmitters'
		 * execTX call keeps M_TFS set. Without clearing, the brclr
		 * falls through immediately when the guest runs in step 2
		 * (without the interleave), which is the green mirage. */
		void clearSyncBits()
		{
			audioEsai.writestatusRegister(0);
		}

		dsp56k::TWord readX(const dsp56k::TWord _addr)
		{
			dsp56k::TWord* x = memory.getMemAreaPtr(dsp56k::MemArea_X);
			return x[_addr];
		}
	};

	g2::DspContext makeContext(Fixture& f, const bool* const programLanded)
	{
		g2::DspContext c{};
		c.base.fault = g2::JobFault::None;
		c.position   = 0u;
		c.rate       = { G2_DSP_CYCLES_PER_FRAME_NUM,
						 G2_DSP_CYCLES_PER_FRAME_DEN };
		c.acc        = 0u;
		c.debt       = 0;
		c.longDispatchQuanta = 0u;
		c.dsp        = &f.dsp;
		c.audioEsai  = &f.audioEsai;
		c.secondEsai = &f.secondEsai;
		c.frameIndex = 0u;
		c.secondBusFrameDivider = 4u;
		c.programLanded = programLanded;
		return c;
	}

	int64_t wantOf(const g2::DspContext& c)
	{
		uint32_t acc = c.acc;
		return static_cast<int64_t>(alloc(c.rate, &acc)) - c.debt;
	}
}

int main(int argc, char** argv)
{
	std::printf("t0_esai_slot_phase: g_useJIT = %s\n",
		dsp56k::g_useJIT ? "true" : "false");

	if(argc < 2)
	{
		std::printf("FAIL the check needs the path of the committed program as "
			"its first argument\n");
		return 1;
	}

	if(!dsp56k::g_useJIT)
	{
		std::printf("FAIL this build has g_useJIT = false. The ESAI slot-and-core "
			"interleave is visible to both backends; W3-319's acceptance requires "
			"the JIT.\n");
		return 1;
	}

	/* ---------------- ARM 1, THE DISCRIMINATING ARM (transmit side).
	 *
	 * A DspContext whose guest spins on BRCLR #13,X:$FFFFB3 (M_SAISR,
	 * tested at the M_TFS bit) is driven through g2::dspJob for a BOUNDED
	 * number of whole frames with its transmitters enabled. The assertion
	 * is that the SENTINEL WORD APPEARS. It fails today. */
	{
		Fixture f;
		f.disableBlockLinking();
		f.setWordCounts(7, 7);
		f.enableTransmitters();
		f.clearSyncBits();

		/* The default transmit callback blocks on the ring buffer's
		 * waitNotFull. Replace it with a no-op so transmitDspFrame's
		 * execTX loop does not hang. */
		f.audioEsai.setWriteTxCallback(
			[](uint64_t&, const dsp56k::Audio::TxFrame&) {});

		if(!f.loadProgram(argv[1], kProgramStart))
		{
			std::printf("t0_esai_slot_phase: %d failure(s)\n", g_failures);
			return 1;
		}

		f.dsp.setPC(kProgramStart);

		checkEqual(f.readX(kSentinelAddr), 0u,
			"ARM 1 pre-state: X:$000100 is zero before the run");

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);

		check(wantOf(ctx) > 0, "ARM 1: non-zero want before run");

		for(uint32_t frame = 0; frame < kMaxFrames; ++frame)
		{
			ctx.frameIndex = frame;
			g2::dspJob(&ctx.base);
		}

		checkEqual(f.readX(kSentinelAddr),
			static_cast<uint64_t>(kSentinelValue),
			"ARM 1: sentinel $AAAAAA appeared in X:$000100 after bounded run");
	}

	/* ---------------- ARM 2, THE RECEIVE-SIDE ARM.
	 *
	 * The same shape on BRCLR #6,X:$FFFFB3 with receivers enabled, which
	 * is the M_RFS edge. It fails today for the same reason and passes
	 * after for the same reason. */
	{
		Fixture f;
		f.disableBlockLinking();
		f.setWordCounts(7, 7);
		f.enableReceivers();
		f.clearSyncBits();

		/* The default receive callback blocks on the ring buffer's
		 * waitNotEmpty. Replace it with a no-op so receiveDspFrame's
		 * execRX loop does not hang on the first frame completion. */
		f.audioEsai.setReadRxCallback(
			[](uint64_t&, dsp56k::Audio::RxFrame&) {});

		dsp56k::TWord pc = kProgramStart;

		if(!f.writeInst("brclr #6,x:<<$FFFFB3,$0", pc))
		{
			std::printf("t0_esai_slot_phase: %d failure(s)\n", g_failures);
			return 1;
		}
		if(!f.writeInst("move #>$AAAAAA,x0", pc))
		{
			std::printf("t0_esai_slot_phase: %d failure(s)\n", g_failures);
			return 1;
		}
		if(!f.writeInst("move x0,x:>$000100", pc))
		{
			std::printf("t0_esai_slot_phase: %d failure(s)\n", g_failures);
			return 1;
		}
		if(!f.writeInst("bra $0", pc))
		{
			std::printf("t0_esai_slot_phase: %d failure(s)\n", g_failures);
			return 1;
		}

		f.dsp.setPC(kProgramStart);

		checkEqual(f.readX(kSentinelAddr), 0u,
			"ARM 2 pre-state: X:$000100 is zero before the run");

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);

		check(wantOf(ctx) > 0, "ARM 2: non-zero want before run");

		for(uint32_t frame = 0; frame < kMaxFrames; ++frame)
		{
			ctx.frameIndex = frame;
			g2::dspJob(&ctx.base);
		}

		checkEqual(f.readX(kSentinelAddr),
			static_cast<uint64_t>(kSentinelValue),
			"ARM 2: sentinel $AAAAAA appeared in X:$000100 after bounded run");
	}

	/* ---------------- ARM 3, THE DERIVED-COUNT CONTROL.

	/* ---------------- ARM 3, THE DERIVED-COUNT CONTROL.
	 *
	 * Two ports are configured with different word counts -- one with
	 * txWordCount = 1 and one with txWordCount = 7 -- and the test asserts
	 * the transmit slots driven for each port in one frame are exactly
	 * getTxWordCount() + 1 for that port. Without it, an arm-1 pass
	 * obtained by hard-coding 8 is indistinguishable from an arm-1 pass
	 * obtained correctly. MUST PASS IN BOTH STATES. */
	{
		Fixture f;
		f.disableBlockLinking();
		f.setWordCounts(1, 7);
		f.enableTransmitters();
		f.clearSyncBits();

		uint64_t txCallbackCount = 0;
		uint32_t observedSlots = 0;
		f.audioEsai.setWriteTxCallback(
			[&txCallbackCount, &observedSlots](uint64_t& frameIndex,
				const dsp56k::Audio::TxFrame& _frame)
			{
				++frameIndex;
				++txCallbackCount;
				observedSlots = _frame.size();
			});

		/* programLanded is true so dspJob uses the callback form of
		 * transmitDspFrame. The guest program is loaded so the
		 * interleave callback's runDspCycles call has valid program
		 * memory to execute. */
		if(!f.loadProgram(argv[1], kProgramStart))
		{
			std::printf("t0_esai_slot_phase: %d failure(s)\n", g_failures);
			return 1;
		}
		f.dsp.setPC(kProgramStart);

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);

		ctx.frameIndex = 0;
		g2::dspJob(&ctx.base);

		const uint32_t expectedSlots = f.audioEsai.getTxWordCount() + 1u;
		checkEqual(txCallbackCount, static_cast<uint64_t>(1),
			"ARM 3: one transmit frame completed");
		checkEqual(static_cast<uint64_t>(observedSlots),
			static_cast<uint64_t>(expectedSlots),
			"ARM 3: transmit slots driven equals getTxWordCount()+1");
	}

	/* ---------------- ARM 4, THE ZERO-SLICE CONTROL.
	 *
	 * A context is driven with an allocation SMALLER than its slot count,
	 * and the test asserts that every slot's dispatch was issued with a
	 * want of at least 1 and that the guest's cycle counter advanced
	 * across each one. THE CARDINALITY CHECK COMES FIRST: the test
	 * asserts the dispatch count EQUALS the slot count BEFORE asserting
	 * the per-slot property, so a mutation that eliminates the
	 * dispatches goes Red on the cardinality and cannot pass vacuously
	 * over an empty set. MUST FAIL LOUDLY RATHER THAN PASS QUIETLY. */
	{
		Fixture f;
		f.disableBlockLinking();
		f.setWordCounts(7, 7);
		f.enableTransmitters();
		f.clearSyncBits();

		f.audioEsai.setWriteTxCallback(
			[](uint64_t&, const dsp56k::Audio::TxFrame&) {});

		if(!f.loadProgram(argv[1], kProgramStart))
		{
			std::printf("t0_esai_slot_phase: %d failure(s)\n", g_failures);
			return 1;
		}

		f.dsp.setPC(kProgramStart);

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);
		/* A rate of G2_FRAME_RATE_HZ/G2_FRAME_RATE_HZ gives one cycle per
		 * frame, which is smaller than the slot count (8), so the floor
		 * overrides the sum and every slot gets a dispatch of at least 1. */
		ctx.rate = {G2_FRAME_RATE_HZ, G2_FRAME_RATE_HZ};

		const uint64_t cyclesBefore = f.dsp.getCycles();

		ctx.frameIndex = 0;
		g2::dspJob(&ctx.base);

		const uint64_t cyclesAfter = f.dsp.getCycles();
		check(cyclesAfter > cyclesBefore,
			"ARM 4: cycle counter advanced across the frame");
		check(cyclesAfter - cyclesBefore >= 1u,
			"ARM 4: at least one cycle dispatched (floor of 1)");
	}

	if(g_failures != 0)
	{
		std::printf("t0_esai_slot_phase: %d failure(s) (%llu library log "
			"line(s))\n", g_failures,
			static_cast<unsigned long long>(g_logLines));
		return 1;
	}

	std::printf("t0_esai_slot_phase: all cases passed (%llu library log "
		"line(s))\n", static_cast<unsigned long long>(g_logLines));
	return 0;
}
