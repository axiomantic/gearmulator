/* t0_esai_idle_core.cpp -- the check of task SCH-35. Design 13.10.3, 13.4.6.
 *
 * TWO CASES, AND ONE UNIT IS ONE CASE.
 *
 * CASE 1 IS THE DISCRIMINATING CASE. Both ESAI ports sit at their reset
 * state -- no fixture-side enable call of any kind -- and the assertion is
 * that the slot's cycle counter ADVANCES across the quantum. Against the
 * unrepaired dspJob it stays at zero, because both frame helpers return
 * before their loops and the interleave callbacks never fire. The debt
 * assertion ties the direct run to the single reconciliation: debt equals
 * (cycle delta - want) exactly, so a direct run that bypassed totalSpent
 * fails here and not in production.
 *
 * CASE 2 IS THE COMPANION CASE. Transmitters AND receivers are enabled
 * through the public control-register writes, and the guest spins on the
 * receive frame-sync bit (M_RFS). The sync edge exists only BETWEEN slots,
 * so the sentinel appears only when the interleave runs the guest between
 * execRX calls. THE FRAME-CALLBACK COUNTS ALONE DO NOT CATCH ITS MUTATION:
 * the bare helpers still complete frames, so a dspJob that deleted the
 * callback wiring and went frame-granular keeps both counts at two per two
 * quanta. The sentinel assertion is what catches that mutation; the counts
 * pin the frame cadence around it.
 *
 * THE CYCLE ASSERTIONS USE A LOWER BAND AND NEVER AN EQUALITY: runDspCycles
 * tests the counter BEFORE each exec(), so the last dispatch unit carries it
 * past the want, and the dispatch unit is the fixture's own measured cost,
 * not a threshold any build symbol names. want itself is computed here by
 * the same formula dspJob uses, from the context's own fields.
 *
 * THE JOB IS ENTERED THROUGH THE JobContext* RECOVERY THE EXECUTOR USES, on
 * SCH-33's precedent, and the program sits BELOW Vba_End so
 * dynamicFastInterrupts (set in DspSet::Slot::Slot) makes exec() return
 * after each instruction. The test fails loudly rather than skipping on a
 * non-JIT build.
 */

#include "dspContext.h"
#include "dspSet.h"

#include "g2/timebase.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/interrupts.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals56311.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace g2
{
	void dspJob(JobContext*) noexcept;
}

namespace
{
	int g_failures = 0;

	void check(const bool _cond, const char* const _what)
	{
		if(_cond)
		{
			std::printf("ok   %s\n", _what);
			return;
		}
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

	uint64_t g_logLines = 0;

	void countLogLine(const std::string&)
	{
		++g_logLines;
	}

	constexpr dsp56k::TWord kSentinelAddr  = 0x000100;
	constexpr dsp56k::TWord kSentinelValue = 0xAAAAAA;

	/* BELOW Vba_End ($100), so dynamicFastInterrupts puts the JIT in
	 * FastInterruptMode::Dynamic and exec() returns after each instruction. */
	constexpr dsp56k::TWord kProgramStart = 0x0080;

	static_assert(kProgramStart < dsp56k::Vba_End,
		"the fixture spin must sit below Vba_End so the JIT returns after "
		"each instruction");

	constexpr uint32_t kQuanta = 2;

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

		bool writeInst(const char* _text, dsp56k::TWord& _pc)
		{
			const dsp56k::Assembler assembler;
			const auto r = assembler.assemble(_text);
			if(!r.success())
			{
				std::printf("FAIL could not assemble \"%s\"\n", _text);
				++g_failures;
				return false;
			}
			for(uint32_t i = 0; i < r.wordCount; ++i)
			{
				if(!dsp.memWriteP(_pc, r.word[i]))
				{
					std::printf("FAIL could not write program word\n");
					++g_failures;
					return false;
				}
				++_pc;
			}
			return true;
		}

		bool loadSpin(const bool _receiveSide)
		{
			dsp56k::TWord pc = kProgramStart;
			const char* spin = _receiveSide
				? "brclr #6,x:<<$FFFFB3,$0"   /* M_RFS */
				: "brclr #13,x:<<$FFFFB3,$0"; /* M_TFS */
			if(!writeInst(spin, pc))
				return false;
			if(!writeInst("move #>$AAAAAA,x0", pc))
				return false;
			if(!writeInst("move x0,x:>$000100", pc))
				return false;
			if(!writeInst("bra $0", pc))
				return false;
			dsp.setPC(kProgramStart);
			return true;
		}

		void setWordCounts(const uint32_t _tx, const uint32_t _rx)
		{
			audioEsai.writeTransmitClockControlRegister(
				(_tx << dsp56k::Esai::M_TDC0) & dsp56k::Esai::M_TDC);
			audioEsai.writeReceiveClockControlRegister(
				(_rx << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);
		}

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

	/* want, computed by the same formula dspJob uses, from the context's
	 * own fields. The accumulator copy keeps makeContext's acc at zero. */
	int64_t wantOf(const g2::DspContext& c)
	{
		uint32_t acc = c.acc;
		return static_cast<int64_t>(alloc(c.rate, &acc)) - c.debt;
	}

	void runQuanta(g2::DspContext& ctx)
	{
		for(uint32_t q = 0; q < kQuanta; ++q)
		{
			ctx.frameIndex = q;
			g2::dspJob(&ctx.base);
		}
	}
}

int main()
{
	std::printf("t0_esai_idle_core: g_useJIT = %s\n",
		dsp56k::g_useJIT ? "true" : "false");

	if(!dsp56k::g_useJIT)
	{
		std::printf("FAIL this build has g_useJIT = false. The idle-route "
			"delivery is visible to both backends; W3-335's acceptance "
			"requires the JIT.\n");
		return 1;
	}

	/* ---------------- CASE 1, THE DISCRIMINATING CASE (both ports idle).
	 *
	 * NO ENABLE CALL EXISTS IN THIS CASE, fixture-side or guest-side. Both
	 * ports are read back to prove the reset state before the run: if
	 * either direction reported enabled, the case would exercise the
	 * interleave and discriminate nothing. */
	{
		Fixture f;

		checkEqual(f.audioEsai.hasEnabledTransmitters(), 0u,
			"CASE 1 pre-state: audio port has no enabled transmitters");
		checkEqual(f.audioEsai.hasEnabledReceivers(), 0u,
			"CASE 1 pre-state: audio port has no enabled receivers");
		checkEqual(f.secondEsai.hasEnabledTransmitters(), 0u,
			"CASE 1 pre-state: second port has no enabled transmitters");
		checkEqual(f.secondEsai.hasEnabledReceivers(), 0u,
			"CASE 1 pre-state: second port has no enabled receivers");

		if(!f.loadSpin(false))
		{
			std::printf("t0_esai_idle_core: %d failure(s)\n", g_failures);
			return 1;
		}

		checkEqual(f.readX(kSentinelAddr), 0u,
			"CASE 1 pre-state: X:$000100 is zero before the run");

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);

		check(wantOf(ctx) > 0, "CASE 1: non-zero want before run");

		/* THE RECONCILIATION IS ASSERTED PER QUANTUM, twice, each against
		 * a want recomputed from the context's LIVE fields before that
		 * quantum runs -- the invariant is debt = spent - want for THAT
		 * quantum, not for their sum. */
		for(uint32_t q = 0; q < kQuanta; ++q)
		{
			ctx.frameIndex = q;

			const int64_t    wantQ  = wantOf(ctx);
			const uint64_t   before = f.dsp.getCycles();

			g2::dspJob(&ctx.base);

			const uint64_t delta = f.dsp.getCycles() - before;

			check(delta >= static_cast<uint64_t>(wantQ),
				"CASE 1: cycle counter advanced by at least want in "
				"quantum");
			checkEqual(ctx.debt,
				static_cast<int64_t>(delta) - wantQ,
				"CASE 1: debt reconciled to cycle delta minus want");
		}

		checkEqual(f.readX(kSentinelAddr),
			static_cast<uint64_t>(kSentinelValue),
			"CASE 1: sentinel $AAAAAA appeared in X:$000100");
	}

	/* ---------------- CASE 2, THE COMPANION CASE (ports enabled).
	 *
	 * Transmitters AND receivers are enabled through the public
	 * control-register writes, the same calls the guest firmware makes.
	 * The guest spins on M_RFS, whose set edge exists only between
	 * execRX calls, so the sentinel separates a slot-interleaved delivery
	 * from a frame-granular one. The two frame-callback counters pin the
	 * cadence: exactly one receive frame and one transmit frame complete
	 * per quantum. */
	{
		Fixture f;
		f.setWordCounts(7, 7);

		f.audioEsai.writeTransmitControlRegister(dsp56k::Esai::M_TEM);
		f.audioEsai.writeReceiveControlRegister(dsp56k::Esai::M_REM);
		f.clearSyncBits();

		uint64_t txFrames = 0;
		uint64_t rxFrames = 0;
		f.audioEsai.setWriteTxCallback(
			[&txFrames](uint64_t&, const dsp56k::Audio::TxFrame&)
			{
				++txFrames;
			});
		f.audioEsai.setReadRxCallback(
			[&rxFrames](uint64_t&, dsp56k::Audio::RxFrame&)
			{
				++rxFrames;
			});

		/* The gate the production code reads is truthiness, and that is
		 * what these two pin: at least one direction enabled on each side.
		 * The exact per-channel encoding is the ESAI library's own. */
		check(f.audioEsai.hasEnabledTransmitters() != 0,
			"CASE 2 pre-state: audio port transmitter enabled");
		check(f.audioEsai.hasEnabledReceivers() != 0,
			"CASE 2 pre-state: audio port receiver enabled");

		if(!f.loadSpin(true))
		{
			std::printf("t0_esai_idle_core: %d failure(s)\n", g_failures);
			return 1;
		}

		checkEqual(f.readX(kSentinelAddr), 0u,
			"CASE 2 pre-state: X:$000100 is zero before the run");

		const bool landed = true;
		g2::DspContext ctx = makeContext(f, &landed);

		const int64_t want = wantOf(ctx);
		check(want > 0, "CASE 2: non-zero want before run");

		const uint64_t cyclesBefore = f.dsp.getCycles();

		runQuanta(ctx);

		const uint64_t cyclesAfter  = f.dsp.getCycles();
		const uint64_t cyclesDelta  = cyclesAfter - cyclesBefore;

		check(cyclesDelta >= static_cast<uint64_t>(want),
			"CASE 2: cycle counter advanced by at least want across the "
			"quanta");
		checkEqual(f.readX(kSentinelAddr),
			static_cast<uint64_t>(kSentinelValue),
			"CASE 2: sentinel $AAAAAA appeared mid-frame via the interleave");
		checkEqual(rxFrames, kQuanta,
			"CASE 2: one receive frame completed per quantum");
		checkEqual(txFrames, kQuanta,
			"CASE 2: one transmit frame completed per quantum");
	}

	if(g_failures != 0)
	{
		std::printf("t0_esai_idle_core: %d failure(s) (%llu library log "
			"line(s))\n", g_failures,
			static_cast<unsigned long long>(g_logLines));
		return 1;
	}

	std::printf("t0_esai_idle_core: all cases passed (%llu library log "
		"line(s))\n", static_cast<unsigned long long>(g_logLines));
	return 0;
}
