/* t0_dsp_job_order.cpp -- the check of task SCH-11. Design 13.10.3, 13.4.6.
 *
 * THE CHECK IS THE ORDER, AND THE ORDER IS STATED AS A LIST BECAUSE THE
 * SAMPLE OFFSET OF DESIGN SECTION 12.3 DEPENDS ON IT AND ON NO OTHER:
 *
 *   1.  receiveDspFrame(audioEsai), and receiveDspFrame(secondEsai) only
 *       when frameIndex % secondBusFrameDivider == 0;
 *   2.  the budget/want/debt block that calls runDspCycles;
 *   3.  transmitDspFrame(audioEsai), and the second bus on the same
 *       condition.
 *
 * STEPS 1 AND 3 RUN EVEN WHEN STEP 2 RUNS NOTHING. The test drives a quantum
 * where the carried debt consumes the whole allocation, so runQuantum takes
 * its want <= 0 branch, executes no emulated cycle and returns 0 -- and one
 * receive and one transmit still happen. That is the property this whole row
 * exists to hold: the scheduler owns the cadence, so a long-dispatch quantum
 * still transmits.
 *
 * WHAT THE FIXTURE IS. dsp56k::Esai needs an IPeripherals and both
 * control-register writes reach through it to the DSP, so the fixture builds
 * a real Memory, two PeripheralsNop and a real DSP -- exactly the shape
 * t0_esai_frame builds. Two Esai objects sit on that one DSP: audioEsai on
 * MemArea_X (the audio bus) and secondEsai on MemArea_Y (ESAI_1). NO
 * EsaiClock IS CONSTRUCTED ANYWHERE IN THIS CHECK, for the reason the whole
 * scheduler exists. The transmit and receive callbacks are the fixture's own
 * and record into a single ordered log, so the check observes the ORDER, not
 * just the counts.
 *
 * THE DEBT-CONSUMED QUANTUM NEVER REACHES THE DSP. runQuantum's want <= 0
 * branch calls no role-filler, so c->dsp is never dereferenced and the
 * context below carries a NULL dsp on purpose: a fixture that needed a live
 * JIT DSP to prove the order would be asserting the order against pieces the
 * row does not own.
 */

#include "dspContext.h"
#include "esaiFrame.h"
#include "executor.h"
#include "cycleDebt.h"

#include "dsp56kBase/logging.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace g2
{
	/* SCH-11's Files: line names dspJob.cpp and this test and no header, so
	 * the declaration is forward-declared here. It is PINNED against the
	 * Executor's JobFn below, so a signature drift is a compile error and not
	 * a silent mismatch. */
	void dspJob(JobContext*) noexcept;
}

/* THE TYPE IS PINNED, AND THAT IS A CHECK IN ITSELF. dspJob must satisfy the
 * Executor's JobFn exactly -- void(JobContext*) noexcept -- or it could not
 * fill one of the jobs the Scheduler builds. A signature drift is a
 * compile error at this line. */
static_assert(std::is_same_v<g2::Executor::JobFn, decltype(&g2::dspJob)>,
	"g2::dspJob must satisfy the Executor's JobFn.");

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

	void checkOrder(const std::vector<std::string>& observed,
		const std::vector<std::string>& expected)
	{
		if(observed != expected)
		{
			printf("FAIL dspJob order:\n  observed:");
			for(const auto& s : observed)
				printf(" %s", s.c_str());
			printf("\n  expected:");
			for(const auto& s : expected)
				printf(" %s", s.c_str());
			printf("\n");
			++failures;
		}
	}

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	/* The run gate in dspJob (SCH-33) reads a BORROWED flag on the context and
	 * a value-initialised context carries NULL, which that gate reads as NOT
	 * landed. The factory below points it at a landed slot, because the
	 * property this file holds is the ORDER around step 2 -- including its
	 * want <= 0 branch -- and not the gate. t0_dsp_run_gate holds the gate. */
	const bool g_programLanded = true;

	struct Fixture
	{
		dsp56k::Memory           memory;
		dsp56k::PeripheralsNop   periphX;
		dsp56k::PeripheralsNop   periphY;
		dsp56k::DSP              dsp;
		dsp56k::Esai             audioEsai;    /* MemArea_X, the audio bus   */
		dsp56k::Esai             secondEsai;   /* MemArea_Y, ESAI_1          */

		std::vector<std::string> order;
		uint64_t audioTx = 0;
		uint64_t audioRx = 0;
		uint64_t secondTx = 0;
		uint64_t secondRx = 0;

		Fixture()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &periphX, &periphY)
			, audioEsai(periphX,  dsp56k::MemArea_X)
			, secondEsai(periphY, dsp56k::MemArea_Y)
		{
			audioEsai.setWriteTxCallback(
				[this](uint64_t& frameIndex, const dsp56k::Audio::TxFrame&)
				{
					++frameIndex;
					order.emplace_back("a_tx");
					++audioTx;
				});
			audioEsai.setReadRxCallback(
				[this](uint64_t& frameIndex, dsp56k::Audio::RxFrame& frame)
				{
					++frameIndex;
					frame.resize(dsp56k::Audio::MaxSlotsPerFrame);
					order.emplace_back("a_rx");
					++audioRx;
				});
			secondEsai.setWriteTxCallback(
				[this](uint64_t& frameIndex, const dsp56k::Audio::TxFrame&)
				{
					++frameIndex;
					order.emplace_back("s_tx");
					++secondTx;
				});
			secondEsai.setReadRxCallback(
				[this](uint64_t& frameIndex, dsp56k::Audio::RxFrame& frame)
				{
					++frameIndex;
					frame.resize(dsp56k::Audio::MaxSlotsPerFrame);
					order.emplace_back("s_rx");
					++secondRx;
				});
		}

		/* Resets the observation baseline WITHOUT disturbing the ESI. The
		 * enable calls below can complete a whole short frame and fire a
		 * callback before the case under test has run one quantum; clearing
		 * after enabling makes the baseline "the enable happened" and nothing
		 * else. */
		void resetBaseline()
		{
			order.clear();
			audioTx = audioRx = secondTx = secondRx = 0;
		}

		void setWordCounts()
		{
			audioEsai.writeTransmitClockControlRegister(
				(7u << dsp56k::Esai::M_TDC0) & dsp56k::Esai::M_TDC);
			audioEsai.writeReceiveClockControlRegister(
				(7u << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);
			secondEsai.writeTransmitClockControlRegister(
				(7u << dsp56k::Esai::M_TDC0) & dsp56k::Esai::M_TDC);
			secondEsai.writeReceiveClockControlRegister(
				(7u << dsp56k::Esai::M_RDC0) & dsp56k::Esai::M_RDC);
		}

		void enableAll()
		{
			audioEsai.writeTransmitControlRegister(dsp56k::Esai::M_TEM);
			audioEsai.writeReceiveControlRegister(dsp56k::Esai::M_REM);
			secondEsai.writeTransmitControlRegister(dsp56k::Esai::M_TEM);
			secondEsai.writeReceiveControlRegister(dsp56k::Esai::M_REM);
		}
	};
}

int main()
{
	/* A DspContext whose carried debt consumes the whole allocation, so the
	 * budget/want/debt block takes its want <= 0 branch and runs nothing. The
	 * dsp member is NULL BY DESIGN: that branch never dereferences it. */
	auto makeContext = [](Fixture& f, const uint64_t frameIndex,
		const unsigned divider, const int64_t debt) -> g2::DspContext
	{
		g2::DspContext c{};
		c.base.fault = g2::JobFault::None;
		c.position   = 0u;
		c.rate       = { G2_DSP_CYCLES_PER_FRAME_NUM,
						 G2_DSP_CYCLES_PER_FRAME_DEN };
		c.acc        = 0u;
		c.debt       = debt;
		c.longDispatchQuanta = 0u;
		c.dsp        = nullptr;
		c.audioEsai  = &f.audioEsai;
		c.secondEsai = &f.secondEsai;
		c.frameIndex = frameIndex;
		c.secondBusFrameDivider = divider;
		c.programLanded = &g_programLanded;
		return c;
	};

	/* ---------------- the debt-consumed quantum, window open.
	 *
	 * frameIndex = 0, divider = 4: 0 % 4 == 0, so the second bus advances.
	 * The whole allocation is consumed by debt, so step 2 runs nothing -- and
	 * one audio receive, one audio transmit, and (in this window) one second
	 * receive and one second transmit all still happen, in the fixed order. */
	{
		Fixture f;
		f.setWordCounts();
		f.enableAll();
		f.resetBaseline();

		g2::DspContext ctx = makeContext(f, 0u, 4u, 1000000);
		g2::dspJob(&ctx.base);

		checkOrder(f.order, { "a_rx", "s_rx", "a_tx", "s_tx" });
		checkEqual(f.audioRx, 1u,
			"audio receive still happens when step 2 runs nothing");
		checkEqual(f.secondRx, 1u,
			"second receive happens inside the advance window");
		checkEqual(f.audioTx, 1u,
			"audio transmit still happens when step 2 runs nothing");
		checkEqual(f.secondTx, 1u,
			"second transmit happens inside the advance window");
		checkEqual(ctx.longDispatchQuanta, 1u,
			"step 2 took the want <= 0 branch on the debt-consumed quantum");
	}

	/* ---------------- the debt-consumed quantum, window CLOSED.
	 *
	 * frameIndex = 1, divider = 4: 1 % 4 != 0, so the second bus does not
	 * advance. The audio bus still advances on both sides of the no-op step
	 * 2, which is the property that a long-dispatch quantum still transmits. */
	{
		Fixture f;
		f.setWordCounts();
		f.enableAll();
		f.resetBaseline();

		g2::DspContext ctx = makeContext(f, 1u, 4u, 1000000);
		g2::dspJob(&ctx.base);

		checkOrder(f.order, { "a_rx", "a_tx" });
		checkEqual(f.audioRx, 1u,
			"audio receive still happens outside the second-bus window");
		checkEqual(f.audioTx, 1u,
			"audio transmit still happens outside the second-bus window");
		checkEqual(f.secondRx, 0u,
			"no second receive outside the advance window");
		checkEqual(f.secondTx, 0u,
			"no second transmit outside the advance window");
		checkEqual(ctx.longDispatchQuanta, 1u,
			"step 2 still took the want <= 0 branch with no second bus");
	}

	/* ---------------- the window is the SCHEDULER'S OWN, at the boundary.
	 *
	 * frameIndex exactly on the divider is a window; one frame later is not.
	 * Both must agree ON THE SAME CONDITION for receive and for transmit --
	 * ChainAdapter::advanceAll and dspJob gate on the identical expression. */
	{
		Fixture f;
		f.setWordCounts();
		f.enableAll();
		f.resetBaseline();

		g2::DspContext in = makeContext(f, 4u, 4u, 1000000);
		g2::dspJob(&in.base);

		checkOrder(f.order, { "a_rx", "s_rx", "a_tx", "s_tx" });
		checkEqual(f.secondRx, 1u,
			"a frameIndex exactly on the divider is inside the window");

		f.resetBaseline();
		g2::DspContext out = makeContext(f, 5u, 4u, 1000000);
		g2::dspJob(&out.base);

		checkOrder(f.order, { "a_rx", "a_tx" });
		checkEqual(f.secondRx, 0u,
			"a frameIndex one past the divider is outside the window");
	}

	if(failures != 0)
	{
		printf("t0_dsp_job_order: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_dsp_job_order: all cases passed\n");
	return 0;
}
