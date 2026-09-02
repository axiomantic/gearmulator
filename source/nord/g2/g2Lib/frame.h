/* frame.h -- g2::Frame and the single conversion point in each direction.
 *
 * The spellings are load-bearing. Both library frame types are nested inside
 * class Audio: the correct names are dsp56k::Audio::TxFrame and
 * dsp56k::Audio::RxFrame. `dsp56k::TxFrame` does not exist at namespace scope
 * and would not compile.
 */

#pragma once

#include <cstdint>
#include <type_traits>

#include "dsp56kEmu/audio.h"

namespace g2
{
	/* One ESAI TDM frame: eight slots of 24-bit audio, sign-extended.
	 * Q23 fixed point: 1.0 is 0x800000. */
	struct Frame
	{
		static constexpr unsigned kSlots = 8;
		int32_t slot[kSlots];
	};

	/* Each asserted property carries its own static_assert, at the
	 * declaration site, so the check travels with the declaration. */
	static_assert(Frame::kSlots == 8,
		"An ESAI TDM frame is eight slots.");
	static_assert(std::is_same_v<decltype(Frame::slot[0]), int32_t&>,
		"Frame::slot carries SIGNED elements. Q23 uses bit 23 as the sign bit.");
	static_assert(sizeof(Frame::slot) == 8 * sizeof(int32_t),
		"Frame::slot is a flat array of eight int32_t.");

	/* The compile-time statement that a reinterpreting cast is impossible.
	 *
	 * The library element type is an unsigned TWord and Frame::slot is a
	 * signed int32_t carrying Q23. A cast that reinterpreted the library's
	 * storage would read every negative Q23 value as a large positive number,
	 * and it would do so with no error at any layer. There is no layout under
	 * which collapsing either conversion body to a cast is legal. */
	static_assert(!std::is_same_v<std::remove_cv_t<dsp56k::TWord>, int32_t>,
		"The library element is unsigned, so both conversions must do real "
		"per-slot work. No reinterpreting cast is legal here.");

	/* The single conversion point in each direction.
	 *
	 * fromEsaiFrame sign-extends from bit 23; toEsaiFrame masks to 24 bits.
	 * A frame whose slot count does not match the position's expectation is a
	 * defect, not a silent truncation.
	 *
	 * `reg` is the ESAI transmit or receive register index within a slot. It
	 * is a parameter rather than a constant because the second bus transmits
	 * on TX2:
	 *
	 *   Bus          Direction  DMA endpoint         reg  Slots
	 *   audio chain  receive    $FFFFA8 = M_RX0      0    8, or 2 at the head
	 *   audio chain  transmit   $FFFFA0 = M_TX0      0    8, or 2 at the tail
	 *   second bus   receive    $FFFF88 = M_RX0_1    0    8 everywhere
	 *   second bus   transmit   $FFFF82 = M_TX2_1    2    8 everywhere
	 *
	 * That asymmetry is real hardware programming: the firmware programs DMA
	 * channel 5 against TX2 and not TX0.
	 *
	 * Every register index other than `reg` is untouched -- the conversion
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
