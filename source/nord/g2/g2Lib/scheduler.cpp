/* scheduler.cpp -- the Scheduler factory and the construction rejections.
 * Task SCH-18. Design sections 13.10 rule 4, 13.4.5, 13.6, 13.6.2 and 13.10.5.
 *
 * `Scheduler::create` is the SINGLE REJECTION POINT. Every rejectable value
 * arrives through `Scheduler::Config`, so no other site has to repeat any of
 * these comparisons and no later object can be built from a Config that failed
 * one.
 *
 * THE ORDER OF THE COMPARISONS IS THE ORDER OF THE PLAN'S REJECTION TABLE, AND
 * THAT IS A DECISION RATHER THAN AN ACCIDENT.
 *
 *   A Config can fail more than one row, and then the reported status is
 *   whichever comparison runs first. The alternative considered was to test the
 *   lookahead bound early, on the ground that it is the most expensive mistake
 *   to ship; it was rejected because the bound reads a value the DSP-count row
 *   has not yet vetted. `D_chain` is `(N - 1) * hopFrames` over the unsigned
 *   members ChainAdapter holds, so a count of 0 makes the subtraction WRAP, the
 *   chain delay enormous and the bound exceeded -- and a factory that tested the
 *   bound first would answer BadLookahead for a Config whose actual defect is
 *   the count. Running the table in order means every value the later rows read
 *   has already been vetted by an earlier one.
 *
 *   WHAT THAT DOES NOT ESTABLISH: it fixes the reported status for a Config
 *   that fails several rows, and it says nothing about which single row a
 *   caller's Config was meant to fail. A caller reading one status back learns
 *   the FIRST defect and not the only one.
 *
 * NO EXCEPTION AND NO ASSERTION ANYWHERE IN THIS FILE. Design section 13.10
 * rule 2 forbids throwing, and a release build removes an assertion, so the
 * rejections have to survive NDEBUG. The return value and the status out-param
 * are the whole observable.
 */

#include "scheduler.h"

#include "board.h"
#include "executor.h"

#include <cstdint>

namespace g2
{
	namespace
	{
		/* Above this the framework silently truncates the reported latency and
		 * logs that audio will be out of sync. §13.6.2. The bound is on the
		 * SUM, and `D_codec` is written as a named term rather than folded away
		 * so that a future decision to model the converter has one place to
		 * change. */
		constexpr uint64_t kMaxTotalLookaheadFrames = 16384;
		constexpr uint64_t kDelayCodecFrames        = 0;

		/* The job array holds the DSP contexts only and is fixed at
		 * `kJobCount`; the panel and the MCU run serially in the Scheduler,
		 * outside the Executor. Every other count is rejected, including the
		 * 4-DSP machine BRD-15 records as a real configuration. */
		constexpr unsigned kFixedDspCount = static_cast<unsigned>(kJobCount);

		/* A value above 1 would give one transmit callback per frame with a
		 * single swap point outside the run phase, so each write would
		 * overwrite the last and the mailbox would drop all but the final frame
		 * of every quantum. */
		constexpr unsigned kFixedFramesPerQuantum = 1;
	}

	std::unique_ptr<Scheduler> Scheduler::create(const Config& _config, Executor& _executor, Board& _board,
		Status& _outStatus)
	{
		/* SCH-19 wires both into the constructed object. SCH-18 rejects on the
		 * Config alone, so neither is read here and neither is validated: a
		 * reference cannot be null, and whether the referent outlives the
		 * Scheduler is a lifetime rule this factory cannot check. */
		(void) _executor;
		(void) _board;

		if(_config.framesPerQuantum != kFixedFramesPerQuantum)
		{
			_outStatus = Status::BadFramesPerQuantum;
			return nullptr;
		}

		/* A hop of zero collapses the hop whatever the override says. Every
		 * mailbox is a delay line of `hopFrames + 1` frames, so a frame written
		 * in one quantum would be taken back in that same quantum and SCH-19's
		 * rule that no job may observe another job's state would break inside
		 * the very object the override exists to exercise. */
		if(_config.hopFrames == 0)
		{
			_outStatus = Status::BadHopFrames;
			return nullptr;
		}

		/* The divider is a DIVISOR. dspContext.h's comment on that member
		 * states this rejection as the reason its modulo cannot divide by
		 * zero, so the override must not be able to hand a zero through. */
		if(_config.secondBusFrameDivider == 0)
		{
			_outStatus = Status::BadDivider;
			return nullptr;
		}

		/* The two equality rows. `Config::secondBusFrameDivider` and
		 * `Config::hopFrames` are the values every consumer actually reads, and
		 * without these a Scheduler built with a divider of 1 against a build
		 * constant of 4 was legal -- a golden render taken through it would pass
		 * the timebase gate while the manifest recorded 4.
		 *
		 * THE OVERRIDE IS THE ONLY WAY PAST THEM, and it does not reach the two
		 * zero rows above. */
		if(!_config.testOverride)
		{
			if(_config.secondBusFrameDivider != G2_SECOND_BUS_FRAME_DIVIDER)
			{
				_outStatus = Status::BadDivider;
				return nullptr;
			}

			if(_config.hopFrames != G2_CHAIN_HOP_FRAMES)
			{
				_outStatus = Status::BadHopFrames;
				return nullptr;
			}
		}

		if(_config.dspCount != kFixedDspCount)
		{
			_outStatus = Status::BadDspCount;
			return nullptr;
		}

		if(_config.dspRate.den == 0 || _config.mcuRate.den == 0)
		{
			_outStatus = Status::BadRational;
			return nullptr;
		}

		/* Read only after the DSP-count row has vetted `dspCount`. The
		 * subtraction is unsigned and its operand types are ChainAdapter's, so
		 * the widening below does not rescue a count of zero -- the wrap happens
		 * first, in `unsigned`, and only the order above keeps it unreachable.
		 *
		 * THE SUM IS ACCUMULATED IN 64 BITS AND NOT IN `unsigned`, and that is a
		 * decision rather than habit. A 32-bit sum of a wrapped delay and a
		 * small lookahead wraps a SECOND time and lands back under the bound, so
		 * a Config that ought to be refused would be accepted here -- a check
		 * that passes for the wrong reason, and one whose silence would look
		 * exactly like a correct answer.
		 *
		 * THE WIDTH IS NOT BOUND BY ANY CHECK AND CANNOT BE, and saying so is
		 * part of the decision. While the count row above holds, no Config that
		 * reaches this line can carry a wrapped delay, so narrowing the sum to
		 * `unsigned` changes no observable of this factory. The width earns its
		 * place only against a future reordering of the rows -- which is the one
		 * event that would make it observable, and the one this file cannot
		 * test for itself. */
		{
			const uint64_t chainDelay = static_cast<uint64_t>(_config.dspCount - 1u) * _config.hopFrames;
			const uint64_t total      = chainDelay + kDelayCodecFrames + _config.lookaheadFrames;

			if(total > kMaxTotalLookaheadFrames)
			{
				_outStatus = Status::BadLookahead;
				return nullptr;
			}
		}

		/* The codec queues carry the capacity `lookaheadFrames + B`, so a B of
		 * zero sizes them by the lookahead alone. */
		if(_config.maxHostBlockFrames == 0)
		{
			_outStatus = Status::BadMaxHostBlock;
			return nullptr;
		}

		/* SCH-17's rule, now reported through a status. `g_useJIT` is a
		 * `static constexpr`, so the second half folds at compile time. */
		if(_config.backend != Backend::Jit || !dsp56k::g_useJIT)
		{
			_outStatus = Status::BadBackend;
			return nullptr;
		}

		_outStatus = Status::Ok;
		return std::unique_ptr<Scheduler>(new Scheduler);
	}
}
