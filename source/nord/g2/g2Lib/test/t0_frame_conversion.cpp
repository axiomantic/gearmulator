/* t0_frame_layout holds the signatures of the two conversion points. This check
 * holds their behaviour:
 *
 *   toEsaiFrame(fromEsaiFrame(x, reg), reg, ...) == x
 *
 * over the full Q23 range at every slot, including both saturation bounds, for
 * dsp56k::Audio::TxFrame and dsp56k::Audio::RxFrame, at every register index in
 * the mapping table, and at slot counts 2 and 8.
 *
 * The mapping table. `reg` is a parameter and not a constant because the second
 * bus transmits on TX2:
 *
 *   Bus          Direction  DMA endpoint         reg  Slots
 *   audio chain  receive    $FFFFA8 = M_RX0      0    8, or 2 at the head
 *   audio chain  transmit   $FFFFA0 = M_TX0      0    8, or 2 at the tail
 *   second bus   receive    $FFFF88 = M_RX0_1    0    8 everywhere
 *   second bus   transmit   $FFFF82 = M_TX2_1    2    8 everywhere
 *
 * So the table names two register indices, 0 and 2, and two slot counts, 2 and
 * 8. Both indices are driven against both frame types: the table gives 2 to
 * the second bus transmit alone, and driving it on the receive side as well
 * costs nothing and closes the case where a body ignored its argument.
 *
 * Three further cases, each of which a careless implementation fails:
 *
 *   1. A library word with bit 23 set converts to a negative int32_t. A raw
 *      cast would invert the whole negative half of the range in silence.
 *   2. No register index other than `reg` is disturbed.
 *   3. The slot-count round trip: fromEsaiFrame reads size() and toEsaiFrame
 *      calls resize(), so a count of 2 does not silently become a count of 8.
 */

#include "frame.h"

#include <cstdint>
#include <cstdio>
#include <tuple>

namespace
{
	int failures = 0;

	void fail(const char* const what)
	{
		printf("FAIL %s\n", what);
		++failures;
	}

	void check(const bool condition, const char* const what)
	{
		if(!condition)
			fail(what);
	}

	/* Q23 is 24 bits, and every bound below is DERIVED from that one figure.
	 * A number written down here would be a second definition of the format. */
	constexpr unsigned      kQ23Bits       = 24;
	constexpr dsp56k::TWord kQ23Count      = dsp56k::TWord(1) << kQ23Bits;
	constexpr dsp56k::TWord kQ23Mask       = kQ23Count - 1u;
	constexpr dsp56k::TWord kQ23SignBit    = dsp56k::TWord(1) << (kQ23Bits - 1);

	/* The two saturation bounds, as library words and as the signed values
	 * they must become. */
	constexpr dsp56k::TWord kMostNegativeWord = kQ23SignBit;
	constexpr dsp56k::TWord kMostPositiveWord = kQ23SignBit - 1u;

	constexpr int32_t kMostNegative = -static_cast<int32_t>(kQ23SignBit);
	constexpr int32_t kMostPositive =  static_cast<int32_t>(kQ23SignBit) - 1;

	/* The register indices the frame mapping covers. */
	constexpr unsigned kRegisters[] = { 0u, 2u };
	constexpr unsigned kRegisterCount =
		static_cast<unsigned>(sizeof(kRegisters) / sizeof(kRegisters[0]));

	/* The slot counts the frame mapping covers. */
	constexpr unsigned kSlotCounts[] = { 2u, 8u };
	constexpr unsigned kSlotCountCount =
		static_cast<unsigned>(sizeof(kSlotCounts) / sizeof(kSlotCounts[0]));

	/* Both indices must be legal on both frame types, or a case below would
	 * be driving an index the library does not carry. */
	static_assert(dsp56k::Audio::TxRegisterCount > 2,
		"the transmit slot carries register index 2, which the second bus "
		"uses.");
	static_assert(dsp56k::Audio::RxRegisterCount > 2,
		"the receive slot carries register index 2.");

	/* The full Q23 sweep.
	 *
	 * One pass over every one of the 16,777,216 Q23 words. Slot k takes the
	 * word (v + k) & mask, so as v sweeps the whole range every SLOT sees
	 * every word, and the pass costs one sweep rather than one for each slot.
	 * The slots also hold DIFFERENT words at every step, which is what makes
	 * a body that read slot 0 and copied it to the rest fail here. */
	template<typename TLibraryFrame>
	void sweepRoundTrip(const unsigned reg, const unsigned slotCount,
		const char* const what)
	{
		TLibraryFrame source;
		TLibraryFrame result;

		for(dsp56k::TWord v = 0; v < kQ23Count; ++v)
		{
			source.resize(slotCount);

			for(unsigned k = 0; k < slotCount; ++k)
				source[k][reg] = (v + k) & kQ23Mask;

			const g2::Frame middle = g2::fromEsaiFrame(source, reg);

			g2::toEsaiFrame(middle, reg, result, slotCount);

			if(result.size() != slotCount)
			{
				printf("FAIL %s: the round trip returned %u slots, not %u, at "
					"word %u\n", what, result.size(), slotCount, v);
				++failures;
				return;
			}

			for(unsigned k = 0; k < slotCount; ++k)
			{
				const dsp56k::TWord expected = (v + k) & kQ23Mask;

				if(result[k][reg] != expected)
				{
					printf("FAIL %s: slot %u round-tripped %06X to %06X\n",
						what, k, expected, result[k][reg]);
					++failures;
					return;
				}
			}

			/* The sign is part of the round trip, not a separate property.
			 * A body that round-tripped the bits while producing the wrong
			 * intermediate would pass the comparison above and would feed
			 * every consumer of g2::Frame a wrong number. */
			for(unsigned k = 0; k < slotCount; ++k)
			{
				const dsp56k::TWord word = (v + k) & kQ23Mask;

				const int32_t expected = (word & kQ23SignBit) != 0u
					? static_cast<int32_t>(word) - static_cast<int32_t>(kQ23Count)
					: static_cast<int32_t>(word);

				if(middle.slot[k] != expected)
				{
					printf("FAIL %s: slot %u read word %06X as %d, not %d\n",
						what, k, word, middle.slot[k], expected);
					++failures;
					return;
				}
			}
		}

		printf("%s: the full Q23 range round-trips at every slot\n", what);
	}

	/* The saturation bounds, asserted by name as well as inside the sweep, so
	 * that a failure at a bound reads as a bound failure. */
	template<typename TLibraryFrame>
	void checkSaturationBounds(const unsigned reg, const char* const what)
	{
		TLibraryFrame source;
		source.resize(g2::Frame::kSlots);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			source[k][reg] = (k % 2u) == 0u ? kMostNegativeWord
			                                : kMostPositiveWord;

		const g2::Frame middle = g2::fromEsaiFrame(source, reg);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			const int32_t expected = (k % 2u) == 0u ? kMostNegative
			                                        : kMostPositive;

			if(middle.slot[k] != expected)
			{
				printf("FAIL %s: the saturation bound at slot %u read as %d, "
					"not %d\n", what, k, middle.slot[k], expected);
				++failures;
				return;
			}
		}

		TLibraryFrame result;
		g2::toEsaiFrame(middle, reg, result, g2::Frame::kSlots);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			if(result[k][reg] != source[k][reg])
			{
				printf("FAIL %s: the saturation bound at slot %u did not "
					"round-trip\n", what, k);
				++failures;
				return;
			}
		}
	}

	/* CASE 2: no register index other than `reg` is disturbed.
	 *
	 * The destination is filled with a sentinel in every register of every
	 * slot, including the slots beyond the count. Only [k][reg] for k below
	 * the count may change. */
	template<typename TLibraryFrame>
	void checkNeighbouringRegisters(const unsigned reg,
		const unsigned slotCount, const char* const what)
	{
		constexpr unsigned registerCount = static_cast<unsigned>(
			std::tuple_size<typename TLibraryFrame::Slot>::value);

		/* A sentinel that is distinct for each (slot, register) pair, so a
		 * body that copied one register over another is caught as well as one
		 * that zeroed it. */
		const auto sentinel = [](const unsigned k, const unsigned r)
		{
			return static_cast<dsp56k::TWord>(
				((k + 1u) * 0x001111u + (r + 1u) * 0x010001u) & kQ23Mask);
		};

		TLibraryFrame destination;
		destination.resize(g2::Frame::kSlots);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			for(unsigned r = 0; r < registerCount; ++r)
				destination[k][r] = sentinel(k, r);
		}

		g2::Frame source{};
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			source.slot[k] = static_cast<int32_t>(k) - 4;

		g2::toEsaiFrame(source, reg, destination, slotCount);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			for(unsigned r = 0; r < registerCount; ++r)
			{
				const bool mayChange = r == reg && k < slotCount;

				if(mayChange)
					continue;

				if(destination[k][r] != sentinel(k, r))
				{
					printf("FAIL %s: slot %u register %u changed from %06X to "
						"%06X, and only register %u below slot %u may\n",
						what, k, r, sentinel(k, r), destination[k][r], reg,
						slotCount);
					++failures;
					return;
				}
			}
		}
	}
}

int main()
{
	/* ---------------- the full sweep, over the whole mapping table. */
	for(unsigned r = 0; r < kRegisterCount; ++r)
	{
		for(unsigned s = 0; s < kSlotCountCount; ++s)
		{
			const unsigned reg       = kRegisters[r];
			const unsigned slotCount = kSlotCounts[s];

			char what[128];

			snprintf(what, sizeof(what),
				"TxFrame, register %u, %u slots", reg, slotCount);
			sweepRoundTrip<dsp56k::Audio::TxFrame>(reg, slotCount, what);

			snprintf(what, sizeof(what),
				"RxFrame, register %u, %u slots", reg, slotCount);
			sweepRoundTrip<dsp56k::Audio::RxFrame>(reg, slotCount, what);
		}
	}

	/* ---------------- both saturation bounds, by name. */
	for(unsigned r = 0; r < kRegisterCount; ++r)
	{
		checkSaturationBounds<dsp56k::Audio::TxFrame>(kRegisters[r],
			"TxFrame saturation");
		checkSaturationBounds<dsp56k::Audio::RxFrame>(kRegisters[r],
			"RxFrame saturation");
	}

	/* ---------------- case 1: bit 23 set means a negative int32_t.
	 *
	 * The whole negative half of the range is asserted to come out negative,
	 * and the whole positive half to come out non-negative. A raw cast would
	 * invert the negative half in silence, and a check that looked only at
	 * the two bounds would not see a body that got the middle wrong. */
	{
		dsp56k::Audio::TxFrame source;
		source.resize(g2::Frame::kSlots);

		bool signWrong = false;

		for(dsp56k::TWord v = 0; v < kQ23Count && !signWrong;
			v += g2::Frame::kSlots)
		{
			for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
				source[k][0] = (v + k) & kQ23Mask;

			const g2::Frame middle = g2::fromEsaiFrame(source, 0u);

			for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			{
				const dsp56k::TWord word = (v + k) & kQ23Mask;
				const bool          negative = (word & kQ23SignBit) != 0u;

				if(negative != (middle.slot[k] < 0))
				{
					printf("FAIL bit 23: word %06X converted to %d, and bit 23 "
						"is %s\n", word, middle.slot[k],
						negative ? "set" : "clear");
					++failures;
					signWrong = true;
					break;
				}
			}
		}

		/* And the two extremes, spelled out, because they are the values a
		 * reinterpreting cast gets most wrong. */
		{
			dsp56k::Audio::TxFrame bounds;
			bounds.resize(2u);
			bounds[0][0] = kMostNegativeWord;
			bounds[1][0] = kMostPositiveWord;

			const g2::Frame middle = g2::fromEsaiFrame(bounds, 0u);

			check(middle.slot[0] == kMostNegative,
				"the most negative Q23 word converts to the most negative "
				"int32_t of the format");
			check(middle.slot[1] == kMostPositive,
				"the most positive Q23 word converts to the most positive "
				"int32_t of the format");
			check(middle.slot[0] < 0,
				"a word with bit 23 set converts to a NEGATIVE int32_t");
		}
	}

	/* ---------------- case 2: no other register is disturbed. */
	for(unsigned r = 0; r < kRegisterCount; ++r)
	{
		for(unsigned s = 0; s < kSlotCountCount; ++s)
		{
			checkNeighbouringRegisters<dsp56k::Audio::TxFrame>(
				kRegisters[r], kSlotCounts[s], "TxFrame neighbours");
			checkNeighbouringRegisters<dsp56k::Audio::RxFrame>(
				kRegisters[r], kSlotCounts[s], "RxFrame neighbours");
		}
	}

	/* ---------------- case 3: the slot-count round trip.
	 *
	 * fromEsaiFrame reads src.size() and toEsaiFrame calls dst.resize(), so a
	 * count of 2 must not silently become a count of 8. The destination starts
	 * at 8 slots, so a body that never resized would pass a check that only
	 * looked at the value of the slots. */
	{
		dsp56k::Audio::TxFrame narrow;
		narrow.resize(2u);
		narrow[0][0] = kMostNegativeWord;
		narrow[1][0] = kMostPositiveWord;

		const g2::Frame middle = g2::fromEsaiFrame(narrow, 0u);

		/* The slots beyond the source's count read as silence and not as
		 * whatever the previous frame left there. */
		for(unsigned k = 2; k < g2::Frame::kSlots; ++k)
		{
			check(middle.slot[k] == 0,
				"a slot beyond the source's count reads as silence");
		}

		dsp56k::Audio::TxFrame wide;
		wide.resize(g2::Frame::kSlots);

		g2::toEsaiFrame(middle, 0u, wide, 2u);

		check(wide.size() == 2u,
			"a count of 2 does not silently become a count of 8");

		/* And the other direction, so the resize is known to move both ways. */
		dsp56k::Audio::TxFrame grown;
		grown.resize(2u);

		g2::toEsaiFrame(middle, 0u, grown, g2::Frame::kSlots);

		check(grown.size() == g2::Frame::kSlots,
			"a count of 8 does not silently stay at 2");

		/* The count also survives the round trip at every count the table
		 * names. */
		for(unsigned s = 0; s < kSlotCountCount; ++s)
		{
			const unsigned slotCount = kSlotCounts[s];

			dsp56k::Audio::RxFrame source;
			source.resize(slotCount);
			for(unsigned k = 0; k < slotCount; ++k)
				source[k][0] = (k + 1u) * 0x000101u;

			dsp56k::Audio::RxFrame result;
			result.resize(slotCount == 2u ? g2::Frame::kSlots : 2u);

			g2::toEsaiFrame(g2::fromEsaiFrame(source, 0u), 0u, result,
				source.size());

			check(result.size() == slotCount,
				"the slot count survives the round trip");
		}
	}

	if(failures != 0)
	{
		printf("t0_frame_conversion: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_frame_conversion: all cases passed\n");
	return 0;
}
