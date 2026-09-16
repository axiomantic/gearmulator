/* t0_frame_layout.cpp
 *
 * The things asserted below fail in different ways on purpose.
 *
 * The layout properties are asserted by static_assert inside frame.h, at the
 * declaration site, so that every consumer of the header carries them and not
 * only this test.
 *
 * The signatures are asserted here, by taking the address of each conversion
 * overload through its fully qualified function-pointer type. That spelling is
 * what makes a wrong signature a compile error rather than a silent match
 * against some other overload: remove any one parameter from any one of the
 * types below and no overload matches, so the initialiser fails. A missing
 * definition is then a link error, because main() calls through them all.
 */

#include "frame.h"

#include <cstdio>

/* ---------------- the signatures.
 *
 * `reg` is a PARAMETER and not a constant because the second bus transmits on
 * TX2 while everything else uses register 0.
 */

static constexpr g2::Frame (*p1)(const dsp56k::Audio::TxFrame&, unsigned) noexcept
	= &g2::fromEsaiFrame;
static constexpr void (*p2)(const g2::Frame&, unsigned,
                            dsp56k::Audio::TxFrame&, unsigned) noexcept
	= &g2::toEsaiFrame;

static constexpr g2::Frame (*p3)(const dsp56k::Audio::RxFrame&, unsigned) noexcept
	= &g2::fromEsaiFrame;
static constexpr void (*p4)(const g2::Frame&, unsigned,
                            dsp56k::Audio::RxFrame&, unsigned) noexcept
	= &g2::toEsaiFrame;

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

	/* The Q23 corners. 0x800000 is -1.0 and 0x7FFFFF is just under +1.0.
	 * 0x800000 is the case a reinterpreting cast gets wrong: read as
	 * an unsigned TWord it is +8,388,608, and read as Q23 it is -8,388,608. */
	constexpr dsp56k::TWord kWords[8] =
	{
		0x000000u, 0x000001u, 0x7FFFFFu, 0x800000u,
		0xFFFFFFu, 0x400000u, 0xC00000u, 0x123456u
	};

	constexpr int32_t kExpected[8] =
	{
		0, 1, 8388607, -8388608,
		-1, 4194304, -4194304, 0x123456
	};
}

int main()
{
	/* ---------------- the transmit direction, register 0.
	 *
	 * The audio chain's DMA runs against M_TX0 and M_RX0, so register 0 is the
	 * chain's index in both directions. */
	{
		dsp56k::Audio::TxFrame tx;
		tx.resize(g2::Frame::kSlots);
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			tx[k][0] = kWords[k];

		const g2::Frame f = p1(tx, 0);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			check(f.slot[k] == kExpected[k], "TxFrame sign-extends from bit 23");

		/* The single case the whole conversion exists for. */
		check(f.slot[3] < 0, "a library word with bit 23 set converts NEGATIVE");

		dsp56k::Audio::TxFrame back;
		p2(f, 0, back, g2::Frame::kSlots);

		check(back.size() == g2::Frame::kSlots, "toEsaiFrame resizes the destination");
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			check(back[k][0] == kWords[k], "the Tx round trip is exact");
	}

	/* ---------------- the receive direction, register 0. */
	{
		dsp56k::Audio::RxFrame rx;
		rx.resize(g2::Frame::kSlots);
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			rx[k][0] = kWords[k];

		const g2::Frame f = p3(rx, 0);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			check(f.slot[k] == kExpected[k], "RxFrame sign-extends from bit 23");

		dsp56k::Audio::RxFrame back;
		p4(f, 0, back, g2::Frame::kSlots);

		check(back.size() == g2::Frame::kSlots, "toEsaiFrame resizes the destination");
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			check(back[k][0] == kWords[k], "the Rx round trip is exact");
	}

	/* ---------------- the second bus transmits on TX2, which is why `reg` is a
	 * parameter. Every register other than `reg` must be left alone: the
	 * conversion writes only [k][reg], and a frame handed to toEsaiFrame must
	 * already be zeroed elsewhere. */
	{
		g2::Frame f{};
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			f.slot[k] = kExpected[k];

		dsp56k::Audio::TxFrame dst;
		dst.resize(g2::Frame::kSlots);
		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
			for(unsigned r = 0; r < dsp56k::Audio::TxRegisterCount; ++r)
				dst[k][r] = 0xA5A5A5u;

		p2(f, 2, dst, g2::Frame::kSlots);

		for(unsigned k = 0; k < g2::Frame::kSlots; ++k)
		{
			check(dst[k][2] == kWords[k], "TX2 carries the converted value");

			for(unsigned r = 0; r < dsp56k::Audio::TxRegisterCount; ++r)
			{
				if(r == 2)
					continue;
				check(dst[k][r] == 0xA5A5A5u, "no register other than reg is disturbed");
			}
		}
	}

	/* ---------------- the slot count travels with the frame. The chain head
	 * and tail run 2 slots where every other position runs 8. */
	{
		dsp56k::Audio::TxFrame tx;
		tx.resize(2);
		tx[0][0] = 0x800000u;
		tx[1][0] = 0x7FFFFFu;

		const g2::Frame f = p1(tx, 0);
		check(f.slot[0] == -8388608, "a 2-slot frame converts its first slot");
		check(f.slot[1] == 8388607, "a 2-slot frame converts its second slot");
		check(f.slot[2] == 0, "the slots beyond the count are zeroed, not stale");

		dsp56k::Audio::TxFrame back;
		p2(f, 0, back, 2);
		check(back.size() == 2, "toEsaiFrame carries the slot count it is given");
	}

	if(failures != 0)
	{
		printf("t0_frame_layout: %d check(s) failed\n", failures);
		return 1;
	}

	printf("t0_frame_layout: all checks passed\n");
	return 0;
}
