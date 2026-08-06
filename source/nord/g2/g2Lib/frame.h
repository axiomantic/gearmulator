/* frame.h -- g2::Frame and the single conversion point in each direction.
 * Task SCH-4. Design section 13.10.1.
 *
 * THE SPELLINGS ARE LOAD-BEARING. Both library frame types are nested inside
 * class Audio: the correct names are dsp56k::Audio::TxFrame and
 * dsp56k::Audio::RxFrame. `dsp56k::TxFrame` does not exist at namespace scope
 * and would not compile -- an earlier design draft used that name four times.
 *
 * NOTHING ELSE IN g2Lib NAMES A dsp56k FRAME TYPE. Confining the library type
 * to one conversion point in each direction is what limited the damage of
 * getting that type wrong to two signatures.
 */

#pragma once

#include <cstdint>
#include <type_traits>

#include "dsp56kEmu/audio.h"

namespace g2
{
	/* One ESAI TDM frame: eight slots of 24-bit audio, sign-extended.
	 * Q23 fixed point, AGENTS.md section 2.3. 1.0 is 0x800000.
	 * Trivially copyable. No invariant. Passed by const reference. */
	struct Frame
	{
		static constexpr unsigned kSlots = 8;
		int32_t slot[kSlots];
	};

	/* EVERY ASSERTED PROPERTY CARRIES ITS OWN static_assert, and that is what
	 * makes the check able to fail. They live here, at the declaration site,
	 * so that every consumer of this header carries them and not only the
	 * test. */
	static_assert(Frame::kSlots == 8,
		"An ESAI TDM frame is eight slots.");
	static_assert(std::is_same_v<decltype(Frame::slot[0]), int32_t&>,
		"Frame::slot carries SIGNED elements. Q23 uses bit 23 as the sign bit.");
	static_assert(sizeof(Frame::slot) == 8 * sizeof(int32_t),
		"Frame::slot is a flat array of eight int32_t.");

	/* THE COMPILE-TIME STATEMENT OF "A REINTERPRETING CAST IS IMPOSSIBLE".
	 *
	 * The library element type is an unsigned TWord and Frame::slot is a
	 * signed int32_t carrying Q23. A cast that reinterpreted the library's
	 * storage would read every negative Q23 value as a large positive number,
	 * and it would do so with no error at any layer.
	 *
	 * An earlier design draft hoped both conversion bodies would "collapse to
	 * a cast, and a static_assert in this header records that". That sentence
	 * is DELETED rather than softened: there is no layout under which the
	 * collapse is legal, and a static_assert that appeared to permit one would
	 * be a loaded gun. This assertion says the opposite of that one. */
	static_assert(!std::is_same_v<std::remove_cv_t<dsp56k::TWord>, int32_t>,
		"The library element is unsigned, so both conversions must do real "
		"per-slot work. No reinterpreting cast is legal here.");

	/* The single conversion point in each direction.
	 *
	 * Both bodies ALWAYS do real per-slot work. fromEsaiFrame SIGN-EXTENDS
	 * FROM BIT 23. toEsaiFrame MASKS TO 24 BITS.
	 *
	 * Both also carry the slot count: fromEsaiFrame reads src.size() and
	 * toEsaiFrame calls dst.resize(slotCount). A frame whose count does not
	 * match the position's expectation is a DEFECT, not a silent truncation.
	 *
	 * `reg` is the ESAI transmit or receive register index within a slot, and
	 * it is a PARAMETER rather than a constant BECAUSE THE SECOND BUS
	 * TRANSMITS ON TX2:
	 *
	 *   Bus          Direction  DMA endpoint         reg  Slots
	 *   audio chain  receive    $FFFFA8 = M_RX0      0    8, or 2 at the head
	 *   audio chain  transmit   $FFFFA0 = M_TX0      0    8, or 2 at the tail
	 *   second bus   receive    $FFFF88 = M_RX0_1    0    8 everywhere
	 *   second bus   transmit   $FFFF82 = M_TX2_1    2    8 everywhere
	 *
	 * That asymmetry is real hardware programming -- the firmware programs DMA
	 * channel 5 against TX2 and not TX0 -- and it is easy to miss.
	 *
	 * EVERY REGISTER INDEX OTHER THAN `reg` IS UNTOUCHED: the conversion
	 * writes only [k][reg], so a frame handed to toEsaiFrame must already be
	 * zeroed elsewhere.
	 */
	g2::Frame fromEsaiFrame(const dsp56k::Audio::TxFrame& src, unsigned reg) noexcept;
	void      toEsaiFrame  (const g2::Frame& src, unsigned reg,
	                        dsp56k::Audio::TxFrame& dst, unsigned slotCount) noexcept;

	g2::Frame fromEsaiFrame(const dsp56k::Audio::RxFrame& src, unsigned reg) noexcept;
	void      toEsaiFrame  (const g2::Frame& src, unsigned reg,
	                        dsp56k::Audio::RxFrame& dst, unsigned slotCount) noexcept;
}
