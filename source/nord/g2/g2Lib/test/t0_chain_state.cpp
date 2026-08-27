/* t0_chain_state.cpp -- STEP 2 of task CHN-9 (the absorbed CHN-14).
 * Design sections 12.3 and 13.10.2.
 *
 * THE ROW, VERBATIM: "The test runs 100 quanta, saves, runs 100 more, loads,
 * runs the same 100 again, and asserts identical mailbox contents and
 * identical counters."
 *
 * WHY THIS FILE CANNOT BE SATISFIED BY AN EMPTY SNAPSHOT. A state round trip
 * over an object whose state never moves is satisfied by a snapshot of zero
 * bytes -- the defect SCH-24's block records against its own earlier form.
 * Three things are asserted here so that this file cannot have that shape:
 *
 *   1. THE STATE MOVES. The digest taken at the SAVE point and the digest
 *      taken after the following 100 quanta must DIFFER. If they do not, the
 *      driver below is not driving anything and every later equality is
 *      vacuous, so that inequality is asserted before any equality is.
 *   2. EVERY COUNTER RISES ABOVE ZERO. underrunFrames, secondBusUnderrunFrames
 *      and phaseErrorFrames are each driven above zero by the driver's own
 *      conditions -- a WITHHELD transmit on a cadence for each bus, and an
 *      off-window second-bus transmit -- so "identical counters" is not
 *      0 == 0.
 *   3. THE IMAGE IS SIZED BY THE STRUCTURE. stateSize() is strictly positive
 *      and grows strictly with the ring depth, so a trio that reported a
 *      zero-byte image cannot pass.
 *
 * THE OBSERVABLES ARE RETURNED VALUES AND NOTHING ELSE, so this file reports
 * identically with and without NDEBUG. No case here is a language assert(),
 * no case catches an exception, and the verdict is main's exit status.
 *
 * MAILBOX CONTENTS ARE READ THROUGH THE ADAPTER'S OWN PUBLIC PATHS. Every
 * audio mailbox 0..N-1 and every second-bus mailbox is read through the
 * position's receive callback; the tail mailbox N is read through
 * extractCodecSink, which is the only public reader of it. Nothing here
 * reaches a private member.
 *
 * THE DRIVER IS A PURE FUNCTION OF (adapter state, frame index, position).
 * Each quantum writes every ESAI's status register explicitly before firing a
 * wrapper and builds each transmit frame from scratch, so replaying quanta
 * 100..199 against a RESTORED adapter must reproduce the first replay exactly.
 * Nothing latches in the emulated peripheral between the two replays.
 */

#include "chainAdapter.h"
#include "frame.h"

#include "dsp56kEmu/audio.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_cases    = 0;

	void check(const bool _condition, const char* const _what)
	{
		++g_cases;

		if(_condition)
			return;

		std::printf("FAIL %s\n", _what);
		++g_failures;
	}

	void checkEqualU64(const uint64_t _observed, const uint64_t _expected, const char* const _what)
	{
		++g_cases;

		if(_observed == _expected)
			return;

		std::printf("FAIL %s: observed %llu, expected %llu\n", _what,
			static_cast<unsigned long long>(_observed),
			static_cast<unsigned long long>(_expected));
		++g_failures;
	}

	dsp56k::DefaultMemoryValidator g_memoryValidator;

	/* One chain position's two real Esai objects (the audio bus on MemArea_X
	 * and the second bus / ESAI_1 on MemArea_Y). The same fixture t0_written_flag
	 * uses, for the same reason: the transmit wrappers read the emulated ESAI's
	 * own transmit-underrun latch, so the flag rule needs a real peripheral
	 * behind it. */
	struct PositionEsai
	{
		dsp56k::Memory         memory;
		dsp56k::PeripheralsNop periphX;
		dsp56k::PeripheralsNop periphY;
		dsp56k::DSP            dsp;
		dsp56k::Esai           audioEsai;
		dsp56k::Esai           secondEsai;

		PositionEsai()
			: memory(g_memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &periphX, &periphY)
			, audioEsai(periphX, dsp56k::MemArea_X)
			, secondEsai(periphY, dsp56k::MemArea_Y)
		{}
	};

	constexpr unsigned kPositions = 8u;
	constexpr unsigned kHopFrames = 2u;
	constexpr unsigned kDivider   = 4u;
	constexpr unsigned kQuanta    = 100u;   /* the row's own count */

	/* The deterministic sample a position transmits into one slot on one
	 * quantum. Distinct for every (bus, frame, position, slot) triple within
	 * the 24-bit field, so a mailbox that held another cell's content is
	 * visible as a wrong value rather than as a coincidence. */
	int32_t sample(const unsigned _bus, const uint64_t _frame, const unsigned _position,
		const unsigned _slot)
	{
		const uint32_t v = 0x100000u
			+ (_bus << 19)
			+ (static_cast<uint32_t>(_frame & 0xFFu) << 8)
			+ (_position << 4)
			+ _slot;
		return static_cast<int32_t>(v & 0x7FFFFFu);
	}

	dsp56k::Audio::TxFrame makeTxFrame(const unsigned _bus, const uint64_t _frame,
		const unsigned _position, const unsigned _reg)
	{
		dsp56k::Audio::TxFrame frame;
		frame.resize(g2::Frame::kSlots);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			/* EVERY register of every slot is written, not only `reg`. The
			 * library frame's storage is not zero initialised, so a slot left
			 * alone would carry indeterminate bits into the conversion. */
			for(unsigned r = 0; r < dsp56k::Audio::TxRegisterCount; ++r)
				frame[k][r] = 0u;

			frame[k][_reg] = static_cast<dsp56k::TWord>(sample(_bus, _frame, _position, k));
		}

		return frame;
	}

	/* THE WHOLE OBSERVABLE STATE OF ONE ADAPTER, read through its public
	 * surface. Mailbox contents first, then the three counters and the two
	 * written flags. */
	struct Digest
	{
		int32_t  audioRead [kPositions][g2::Frame::kSlots]{};
		int32_t  secondRead[kPositions][g2::Frame::kSlots]{};
		int32_t  egress    [g2::Frame::kSlots]{};
		uint64_t underrun      [kPositions]{};
		uint64_t secondUnderrun[kPositions]{};
		uint64_t phaseError    [kPositions]{};
		uint8_t  audioWritten  [kPositions]{};
		uint8_t  secondWritten [kPositions]{};
	};

	/* The sentinel the egress frame is pre-filled with. extractCodecSink writes
	 * slots 0 and 1 and NOTHING else, so a surviving sentinel in slots 2..7 is
	 * the observable of "two slots, not eight". */
	constexpr int32_t kEgressSentinel = 0x0055AAu;

	Digest digestOf(g2::ChainAdapter& _adapter)
	{
		Digest d;

		uint64_t frameIndex = 0u;

		for(unsigned p = 0; p < kPositions; ++p)
		{
			dsp56k::Audio::RxFrame rx;

			_adapter.audioRxCallback(p)(frameIndex, rx);
			for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
				d.audioRead[p][k] = k < rx.size() ? static_cast<int32_t>(rx[k][0]) : -1;

			dsp56k::Audio::RxFrame rx2;
			_adapter.secondRxCallback(p)(frameIndex, rx2);
			for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
				d.secondRead[p][k] = k < rx2.size() ? static_cast<int32_t>(rx2[k][0]) : -1;

			d.underrun[p]       = _adapter.underrunFrames(p);
			d.secondUnderrun[p] = _adapter.secondBusUnderrunFrames(p);
			d.phaseError[p]     = _adapter.phaseErrorFrames(p);
			d.audioWritten[p]   = _adapter.audioWritten(p)  ? 1u : 0u;
			d.secondWritten[p]  = _adapter.secondWritten(p) ? 1u : 0u;
		}

		g2::Frame out;
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			out.slot[k] = kEgressSentinel;

		_adapter.extractCodecSink(out);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			d.egress[k] = out.slot[k];

		return d;
	}

	/* Reports the FIRST difference by name, so a failure says which field of
	 * which position moved rather than "the digests differ". */
	bool digestsEqual(const Digest& _a, const Digest& _b, char* const _where, const size_t _size)
	{
		for(unsigned p = 0; p < kPositions; ++p)
		{
			for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			{
				if(_a.audioRead[p][k] != _b.audioRead[p][k])
				{
					std::snprintf(_where, _size,
						"audio mailbox %u slot %u: 0x%06X vs 0x%06X", p, k,
						static_cast<unsigned>(_a.audioRead[p][k]),
						static_cast<unsigned>(_b.audioRead[p][k]));
					return false;
				}

				if(_a.secondRead[p][k] != _b.secondRead[p][k])
				{
					std::snprintf(_where, _size,
						"second-bus mailbox %u slot %u: 0x%06X vs 0x%06X", p, k,
						static_cast<unsigned>(_a.secondRead[p][k]),
						static_cast<unsigned>(_b.secondRead[p][k]));
					return false;
				}
			}

			if(_a.underrun[p] != _b.underrun[p])
			{
				std::snprintf(_where, _size, "underrunFrames(%u): %llu vs %llu", p,
					static_cast<unsigned long long>(_a.underrun[p]),
					static_cast<unsigned long long>(_b.underrun[p]));
				return false;
			}

			if(_a.secondUnderrun[p] != _b.secondUnderrun[p])
			{
				std::snprintf(_where, _size, "secondBusUnderrunFrames(%u): %llu vs %llu", p,
					static_cast<unsigned long long>(_a.secondUnderrun[p]),
					static_cast<unsigned long long>(_b.secondUnderrun[p]));
				return false;
			}

			if(_a.phaseError[p] != _b.phaseError[p])
			{
				std::snprintf(_where, _size, "phaseErrorFrames(%u): %llu vs %llu", p,
					static_cast<unsigned long long>(_a.phaseError[p]),
					static_cast<unsigned long long>(_b.phaseError[p]));
				return false;
			}

			if(_a.audioWritten[p] != _b.audioWritten[p])
			{
				std::snprintf(_where, _size, "audioWritten(%u): %u vs %u", p,
					_a.audioWritten[p], _b.audioWritten[p]);
				return false;
			}

			if(_a.secondWritten[p] != _b.secondWritten[p])
			{
				std::snprintf(_where, _size, "secondWritten(%u): %u vs %u", p,
					_a.secondWritten[p], _b.secondWritten[p]);
				return false;
			}
		}

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			if(_a.egress[k] != _b.egress[k])
			{
				std::snprintf(_where, _size, "egress slot %u: 0x%06X vs 0x%06X", k,
					static_cast<unsigned>(_a.egress[k]),
					static_cast<unsigned>(_b.egress[k]));
				return false;
			}
		}

		std::snprintf(_where, _size, "identical");
		return true;
	}

	/* ONE QUANTUM OF THE FOUR-PHASE PROCEDURE, in section 12.3's order:
	 * swap, ingress, run, egress. The run phase here is synthetic -- every
	 * position's transmit wrapper is fired directly -- because this file is
	 * about what SURVIVES a save and a load and not about who calls the
	 * wrappers.
	 *
	 * THE THREE COUNTER CONDITIONS ARE DRIVEN ON PURPOSE AND EACH ONE IS A
	 * DIFFERENT MECHANISM:
	 *   - the audio transmit is WITHHELD on a 5-quantum cadence, which leaves
	 *     that position's audio flag at "no delivery" and raises
	 *     underrunFrames;
	 *   - the second-bus transmit is WITHHELD on an 11-quantum cadence, which
	 *     raises secondBusUnderrunFrames on a window quantum;
	 *   - a second-bus transmit is forced on a 7-quantum cadence whether or
	 *     not the quantum is a window, which raises phaseErrorFrames.
	 *
	 * THE FIRST TWO USED TO SET M_TUE BY HAND and rely on the wrapper reading
	 * it. That poke is gone, and it is worth saying why rather than just
	 * deleting it: the wrappers never could see M_TUE on a running machine,
	 * because the transmit DMA clears it inside writeSlotToFrame before the
	 * frame it belongs to is delivered. Withholding the transmit drives the
	 * counters through the route that IS reachable from a synthetic driver.
	 * The route where a frame arrives and is stale needs the peripheral's own
	 * transmit path and belongs to t0_esai_underrun_gate, not to a file about
	 * what survives a save and a load. */
	void runQuanta(g2::ChainAdapter& _adapter, PositionEsai* const _esai,
		const uint64_t _from, const unsigned _count)
	{
		for(uint64_t f = _from; f < _from + _count; ++f)
		{
			uint64_t frameIndex = f;

			/* 1. SWAP. */
			_adapter.advanceAll(frameIndex);

			/* 2. INGRESS. */
			g2::Frame src{};
			for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
				src.slot[k] = sample(2u, f, 0u, k);
			_adapter.injectCodecSource(src);

			/* 3. RUN -- every position, ascending. */
			for(unsigned p = 0; p < kPositions; ++p)
			{
				if((f + p) % 5u != 0u)
				{
					const dsp56k::Audio::TxFrame audioTx = makeTxFrame(0u, f, p, 0u);
					_adapter.audioTxCallback(p)(frameIndex, audioTx);
				}

				const bool window   = (f % kDivider) == 0u;
				const bool forced   = (f % 7u) == 0u;

				if(window || forced)
				{
					if((f + p) % 11u != 0u)
					{
						const dsp56k::Audio::TxFrame secondTx = makeTxFrame(1u, f, p, 2u);
						_adapter.secondTxCallback(p)(frameIndex, secondTx);
					}
				}
			}

			/* 4. EGRESS. Read and discarded: this file asserts the egress
			 * content through the digest, and the call is here so the quantum
			 * is the whole four-phase procedure and not three quarters of it. */
			g2::Frame out{};
			_adapter.extractCodecSink(out);
		}
	}
}

int main()
{
	PositionEsai esai[kPositions];

	g2::ChainAdapter adapter(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);

	for(unsigned p = 0; p < kPositions; ++p)
		adapter.attachEsai(p, esai[p].audioEsai, esai[p].secondEsai);

	/* ------------- Case 1: the image is sized by the structure.
	 *
	 * A trio that reported a zero-byte image would satisfy every equality
	 * below, because an empty snapshot restores nothing and changes nothing.
	 * The depth comparison is what makes the size STRUCTURAL rather than a
	 * constant: an image that did not carry the rings cannot grow when the
	 * rings get deeper. */
	{
		const size_t size = adapter.stateSize();

		check(size > 0u,
			"stateSize() is strictly positive (a zero-byte snapshot satisfies "
			"every round trip vacuously)");

		g2::ChainAdapter same(kPositions, kHopFrames, g2::ChainTopology::Ring, kDivider);
		checkEqualU64(same.stateSize(), size,
			"two adapters of the same shape report the same state size");

		g2::ChainAdapter deeper(kPositions, kHopFrames + 1u, g2::ChainTopology::Ring, kDivider);
		check(deeper.stateSize() > size,
			"a deeper ring reports a STRICTLY LARGER state size (the image "
			"carries the mailbox rings)");

		/* stateSize() IS THE FIGURE A CALLER ALLOCATES AGAINST, so a save that
		 * wrote more than it would corrupt whatever follows the buffer, and one
		 * that wrote less would leave a caller's snapshot partly indeterminate.
		 * Both directions are checked, and the check is what keeps the size
		 * expression honest when a field is added to the image:
		 *
		 *   TOO SMALL  the guard region past stateSize() is trampled;
		 *   TOO LARGE  the LAST byte of the image region is never written, and
		 *              it is a written flag -- always 0 or 1, so the fill
		 *              pattern cannot be mistaken for a stored value. */
		constexpr uint8_t kFill  = 0xA5u;
		constexpr size_t  kGuard = 64u;

		std::vector<uint8_t> guarded(size + kGuard, kFill);
		adapter.stateSave(guarded.data());

		uint64_t trampled = 0;
		for(size_t i = size; i < guarded.size(); ++i)
			if(guarded[i] != kFill)
				++trampled;

		checkEqualU64(trampled, 0u,
			"stateSave writes stateSize() bytes and NOT ONE MORE (a size that "
			"under-reports overruns every caller's buffer)");
		check(size > 0u && guarded[size - 1u] != kFill,
			"stateSave writes the LAST byte stateSize() claims (a size that "
			"over-reports leaves a caller's snapshot partly indeterminate)");
	}

	/* ------------- The row's own sequence.
	 *
	 * 100 quanta, save, 100 more, load, the same 100 again. */

	runQuanta(adapter, esai, 0u, kQuanta);

	const Digest atSave = digestOf(adapter);

	std::vector<uint8_t> image(adapter.stateSize());
	adapter.stateSave(image.data());

	runQuanta(adapter, esai, kQuanta, kQuanta);

	const Digest afterSecondRun = digestOf(adapter);

	/* ------------- Case 2: the state MOVES over the second hundred quanta.
	 *
	 * This inequality is asserted BEFORE any equality. Without it, every
	 * "identical" below is satisfied by an adapter that never changes, which
	 * is the empty-snapshot defect this row exists to refuse. */
	{
		char where[256];
		const bool equal = digestsEqual(atSave, afterSecondRun, where, sizeof(where));

		check(!equal,
			"the second hundred quanta MOVE the adapter's state (a digest that "
			"did not move would make every equality below vacuous)");

		if(equal)
			std::printf("     the two digests compared %s\n", where);
	}

	/* ------------- Case 3: every counter rose above zero.
	 *
	 * The totals are COMPUTED over the positions rather than written down, so
	 * a change to the position count cannot leave a stale literal behind. */
	{
		uint64_t underrunTotal = 0, secondUnderrunTotal = 0, phaseErrorTotal = 0;

		for(unsigned p = 0; p < kPositions; ++p)
		{
			underrunTotal       += adapter.underrunFrames(p);
			secondUnderrunTotal += adapter.secondBusUnderrunFrames(p);
			phaseErrorTotal     += adapter.phaseErrorFrames(p);
		}

		check(underrunTotal > 0u,
			"the driver raises underrunFrames above zero (an all-zero counter "
			"makes 'identical counters' mean 0 == 0)");
		check(secondUnderrunTotal > 0u,
			"the driver raises secondBusUnderrunFrames above zero");
		check(phaseErrorTotal > 0u,
			"the driver raises phaseErrorFrames above zero");
	}

	/* ------------- Case 4: load restores the saved state EXACTLY.
	 *
	 * The digest taken immediately after the load must equal the digest taken
	 * at the save point, field for field. This is the direct round trip and it
	 * runs no quanta at all, so nothing but stateSave and stateLoad can make
	 * it pass. */
	adapter.stateLoad(image.data());

	{
		const Digest afterLoad = digestOf(adapter);

		char where[256];
		const bool equal = digestsEqual(atSave, afterLoad, where, sizeof(where));

		check(equal,
			"the digest immediately after stateLoad equals the digest at the "
			"save point (mailbox contents, counters and written flags)");

		if(!equal)
			std::printf("     first difference: %s\n", where);
	}

	/* ------------- Case 5: the row's own assertion.
	 *
	 * Running the SAME hundred quanta against the restored adapter reproduces
	 * the first replay exactly. This is the behavioural half: case 4 says the
	 * bytes came back, and this says the machine they describe runs the same
	 * way afterwards. */
	runQuanta(adapter, esai, kQuanta, kQuanta);

	{
		const Digest replayed = digestOf(adapter);

		char where[256];
		const bool equal = digestsEqual(afterSecondRun, replayed, where, sizeof(where));

		check(equal,
			"replaying the same 100 quanta after the load reproduces identical "
			"mailbox contents and identical counters");

		if(!equal)
			std::printf("     first difference: %s\n", where);
	}

	if(g_failures != 0)
	{
		std::printf("t0_chain_state: %d failure(s) in %d case(s)\n", g_failures, g_cases);
		return 1;
	}

	std::printf("t0_chain_state: all %d cases passed\n", g_cases);
	return 0;
}
