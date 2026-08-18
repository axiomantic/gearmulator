// The host-port callback bridge, from `mc68k::Hdi08` to `dsp56k::HDI08`.
//
// THE INIT CALLBACK SLOT IS DELIBERATELY LEFT ALONE. `setInitHdi08Callback`
// holds one std::function and ASSIGNS it, so a callback installed here would
// silently remove the ICR INIT clear `Hdi08Adapter` installs and return a
// polling host to its spin. A port answers its own init request; the DSP side
// needs nothing at init.
//
// THE STRUCTURE OF `n2xLib/n2xdsp.cpp` TRANSFERS AND ITS THREADING DOES NOT.
// That model runs each DSP on its own thread and blocks waiting for it. The G2
// drives the MCU, the DSPs and the panel from ONE thread, so a blocking wait
// there is a deadlock; the bounded per-quantum word count replaces it.

#pragma once

#include <cstdint>
#include <vector>

#include "mc68k/hdi08.h"

#include "dsp56kEmu/dspBootCode.h"
#include "dsp56kEmu/types.h"

namespace dsp56k
{
	class HDI08;
}

namespace g2
{
	class DspSet;
	class Hdi08Adapter;

	class Hdi08Bridge final
	{
	public:
		Hdi08Bridge(mc68k::Hdi08& _host, dsp56k::DSP& _core, dsp56k::HDI08& _dsp);

		/* NEITHER COPYABLE NOR MOVABLE. The callbacks installed on the host port
		 * capture `this`, so a copy or a move would leave the port driving the
		 * pending backlog of an object the caller no longer holds. */
		Hdi08Bridge(const Hdi08Bridge&) = delete;
		Hdi08Bridge(Hdi08Bridge&&) = delete;
		Hdi08Bridge& operator=(const Hdi08Bridge&) = delete;
		Hdi08Bridge& operator=(Hdi08Bridge&&) = delete;

		/* BORROWED AND NOT COPIED. The scheduler's run gate holds a `const bool*`
		 * for the whole run, so the flag has to be an object with an address and
		 * not a predicate's return value. */
		const bool* programLanded() const noexcept { return &m_programLanded; }

	private:
		void onBootWord(uint32_t _word);
		void onHostWord(uint32_t _word);
		void offerPendingToDsp();
		void drainDspToHost();
		uint8_t mirrorDspHostFlags(uint8_t _isr) const;

		mc68k::Hdi08&  m_host;
		dsp56k::HDI08& m_dsp;

		/* THE BOOT CONSUMER IS THE LIBRARY'S AND NOT `g2::Hdi08Bootstrap`. This
		 * one primes the core -- the counter register, the address register, the
		 * condition codes and the program counter -- and notifies the compiler of
		 * every program-memory write. The G2 model does none of that, so a slot
		 * loaded through it would hold the right words and never run them. */
		dsp56k::DspBoot m_boot;

		bool m_programLanded = false;

		// What the bound would not let through yet. A dropped word is a silent
		// failure; a deferred one is re-offered on the next transfer.
		std::vector<dsp56k::TWord> m_pending;
	};

	// One bridge per slot, host port i to DSP i. The set takes ownership, so the
	// bridges must not outlive the adapter whose ports they drive.
	void attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set);
}
