/* esaiUnderrunPlant.h -- drive a REAL transmit underrun through a REAL
 * dsp56k::Esai, using nothing but the peripheral's public register surface.
 *
 * WHY THIS EXISTS. The chain tests used to put the ESAI into "underrun" by
 * calling writestatusRegister() and setting the M_TUE bit by hand. That poke
 * proves a READ discriminates; it never proves the CONDITION can occur, and it
 * cannot, because the transmit wrappers do not read the bit any more -- the
 * bit does not survive to the instant they run. A fixture that pokes the
 * register passes whichever way the production code is wired, which is how a
 * dead gate survived.
 *
 * WHAT A REAL UNDERRUN IS. Esai::writeSlotToFrame folds the TX registers into
 * the frame being assembled. If the enabled transmitters were not all written
 * since the previous slot, that slot carries stale data: the peripheral logs
 * "ESAI transmit underrun" and raises M_TUE. The frame is not delivered then --
 * it is delivered later, at the frame boundary, from execTX.
 *
 * AND WHY THE BIT IS GONE BY THEN. writeSlotToFrame ends by triggering the
 * transmit DMA, and the DSP (or the DMA on its behalf) answers by writing TX.
 * Esai::writeTX clears M_TUE as soon as every enabled transmitter has been
 * written. So on a running machine the raise and the clear both happen inside
 * one slot, several slots before the frame that carries the stale slot reaches
 * the transmit callback. staleFrame() below reproduces exactly that sequence
 * with a real writeTX rather than a simulated DMA: skip the write for slot 0,
 * then write TX before slot 1, then let the frame complete.
 *
 * THE DRIVER TOUCHES NO PRIVATE STATE. TCCR, TCR, TX and execTX are the
 * peripheral's public surface and are what the firmware uses.
 */

#pragma once

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/esai.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

namespace g2test
{
	/* One chain position's two real Esai objects (the audio bus on MemArea_X
	 * and the second bus / ESAI_1 on MemArea_Y), with the DSP and memory they
	 * need to exist. */
	struct PositionEsai
	{
		dsp56k::DefaultMemoryValidator memoryValidator;
		dsp56k::Memory                 memory;
		dsp56k::PeripheralsNop         periphX;
		dsp56k::PeripheralsNop         periphY;
		dsp56k::DSP                    dsp;
		dsp56k::Esai                   audioEsai;
		dsp56k::Esai                   secondEsai;

		PositionEsai()
			: memory(memoryValidator, 0x080000, 0x800000, 0x200000)
			, dsp(memory, &periphX, &periphY)
			, audioEsai(periphX, dsp56k::MemArea_X)
			, secondEsai(periphY, dsp56k::MemArea_Y)
		{}
	};

	/* Drives one Esai's transmit section. Two slots per frame, one enabled
	 * transmitter (TE0), no transmit interrupts enabled -- the smallest
	 * configuration in which a stale slot and the frame that carries it are
	 * separated in time, which is the whole point. */
	class EsaiTransmitDriver
	{
	public:
		explicit EsaiTransmitDriver(dsp56k::Esai& _esai) : m_esai(_esai) {}

		/* TCCR first: writeTransmitClockControlRegister resets the slot
		 * counter when the word count changes, so enabling the transmitter
		 * afterwards leaves the section at a known slot. TDC = 1 gives
		 * getTxWordCount() == 1 and therefore TWO slots per frame. */
		void enable()
		{
			m_esai.writeTransmitClockControlRegister(1u << dsp56k::Esai::M_TDC0);
			m_esai.writeTransmitControlRegister(1u << dsp56k::Esai::M_TE0);
			syncToFrameBoundary();
		}

		/* Enabling a transmitter runs one slot immediately (the peripheral
		 * starts the section), and that slot underruns because nothing has
		 * been written to TX yet. Finish that frame with TX written, so the
		 * caller starts at slot 0 of a frame with no underrun outstanding. */
		void syncToFrameBoundary()
		{
			const auto frame = m_esai.getTxFrameCounter();

			for(unsigned guard = 0; guard < 64u && m_esai.getTxFrameCounter() == frame; ++guard)
			{
				feed(0x111111u);
				m_esai.execTX();
			}
		}

		/* The DSP's side of the contract: write every enabled transmitter. */
		void feed(const dsp56k::TWord _value) { m_esai.writeTX(0u, _value); }

		/* A frame whose every slot was written in time. No underrun. */
		void goodFrame(const dsp56k::TWord _value)
		{
			feed(_value);
			m_esai.execTX();
			feed(_value);
			m_esai.execTX();
		}

		/* A frame whose FIRST slot underruns for real, and whose second slot
		 * is written in time -- so writeTX clears M_TUE before the frame is
		 * delivered, exactly as the running firmware's DMA does. The frame
		 * that reaches the transmit callback still carries the stale slot. */
		void staleFrame(const dsp56k::TWord _value)
		{
			m_esai.execTX();          /* slot 0: TX not written -> underrun */
			feed(_value);             /* the late refill, which clears M_TUE */
			m_esai.execTX();          /* slot 1 completes and DELIVERS       */
		}

	private:
		dsp56k::Esai& m_esai;
	};
}
