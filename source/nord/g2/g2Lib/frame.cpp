/* frame.cpp -- the conversion functions.
 *
 * The mapping, for a chosen register index R:
 *
 *   g2::Frame::slot[k]  =  signExtend24( libFrame[k][R] )   k = 0 .. size()-1
 *   libFrame[k][R]      =  toUnsigned24( g2::Frame::slot[k] )
 *   libFrame.resize(count)
 */

#include "frame.h"

#include <cassert>
#include <tuple>

namespace g2
{
	namespace
	{
		constexpr dsp56k::TWord kMask24    = 0x00FFFFFFu;
		constexpr dsp56k::TWord kSignBit24 = 0x00800000u;

		/* Q23 carries its sign in bit 23, so a value with that bit set is
		 * negative and every bit above it must be filled in. Done in unsigned
		 * arithmetic and then converted, because a shift of a negative signed
		 * value is not something to rely on. */
		int32_t signExtend24(const dsp56k::TWord word) noexcept
		{
			const dsp56k::TWord masked = word & kMask24;

			if((masked & kSignBit24) == 0u)
				return static_cast<int32_t>(masked);

			return static_cast<int32_t>(masked) - static_cast<int32_t>(kSignBit24 << 1);
		}

		/* The inverse. The mask is what keeps a value that arrived wider than
		 * Q23 from writing into a neighbouring field of the library word. */
		dsp56k::TWord toUnsigned24(const int32_t value) noexcept
		{
			return static_cast<dsp56k::TWord>(value) & kMask24;
		}

		template<typename TLibraryFrame>
		g2::Frame fromEsaiFrameImpl(const TLibraryFrame& src, const unsigned reg) noexcept
		{
			assert(reg < std::tuple_size<typename TLibraryFrame::Slot>::value
				&& "reg is outside the register count of this frame type");

			/* A count that does not match the position's expectation is a
			 * defect. The signature has no error channel and is noexcept, so
			 * the defect is caught in a debug build and the release path
			 * refuses to read past the array rather than corrupting memory. */
			const uint32_t count = src.size();
			assert(count <= g2::Frame::kSlots
				&& "an ESAI frame wider than eight slots reached a G2 position");

			const uint32_t used = count < g2::Frame::kSlots ? count : g2::Frame::kSlots;

			/* Zero-initialised, so the slots beyond the count read as silence
			 * rather than as whatever the previous frame left there. */
			g2::Frame frame{};

			for(uint32_t k = 0; k < used; ++k)
				frame.slot[k] = signExtend24(src[k][reg]);

			return frame;
		}

		template<typename TLibraryFrame>
		void toEsaiFrameImpl(const g2::Frame& src, const unsigned reg,
			TLibraryFrame& dst, const unsigned slotCount) noexcept
		{
			assert(reg < std::tuple_size<typename TLibraryFrame::Slot>::value
				&& "reg is outside the register count of this frame type");
			assert(slotCount <= g2::Frame::kSlots
				&& "a G2 position asked for more slots than a frame carries");

			const unsigned used = slotCount < g2::Frame::kSlots
				? slotCount : g2::Frame::kSlots;

			dst.resize(used);

			for(unsigned k = 0; k < used; ++k)
				dst[k][reg] = toUnsigned24(src.slot[k]);
		}
	}

	g2::Frame fromEsaiFrame(const dsp56k::Audio::TxFrame& src, const unsigned reg) noexcept
	{
		return fromEsaiFrameImpl(src, reg);
	}

	void toEsaiFrame(const g2::Frame& src, const unsigned reg,
		dsp56k::Audio::TxFrame& dst, const unsigned slotCount) noexcept
	{
		toEsaiFrameImpl(src, reg, dst, slotCount);
	}

	g2::Frame fromEsaiFrame(const dsp56k::Audio::RxFrame& src, const unsigned reg) noexcept
	{
		return fromEsaiFrameImpl(src, reg);
	}

	void toEsaiFrame(const g2::Frame& src, const unsigned reg,
		dsp56k::Audio::RxFrame& dst, const unsigned slotCount) noexcept
	{
		toEsaiFrameImpl(src, reg, dst, slotCount);
	}
}
