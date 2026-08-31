// The host-port callback bridge, from `mc68k::Hdi08` to `dsp56k::HDI08`.
//
// The init callback slot is deliberately left alone. `setInitHdi08Callback`
// holds one std::function and assigns it, so a callback installed here would
// silently remove the ICR INIT clear `Hdi08Adapter` installs and return a
// polling host to its spin. A port answers its own init request; the DSP side
// needs nothing at init.
//
// The structure of `n2xLib/n2xdsp.cpp` transfers and its threading does not.
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

		/* The bridge planted the callbacks, so the bridge removes them. Both
		 * ports outlive it -- the adapter's by the ownership rule below, the
		 * DSP's because the set destroys its bridges before its slots -- and a
		 * port left holding a closure over a destroyed bridge calls it on the
		 * next word with nothing to report the fault. */
		~Hdi08Bridge();

		/* Neither copyable nor movable. The callbacks installed on the host port
		 * capture `this`, so a copy or a move would leave the port driving the
		 * pending backlog of an object the caller no longer holds. */
		Hdi08Bridge(const Hdi08Bridge&) = delete;
		Hdi08Bridge(Hdi08Bridge&&) = delete;
		Hdi08Bridge& operator=(const Hdi08Bridge&) = delete;
		Hdi08Bridge& operator=(Hdi08Bridge&&) = delete;

		/* Borrowed and not copied. The scheduler's run gate holds a `const bool*`
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

		/* The core is held and not only forwarded. A host command arrives as a
		 * vector for the core's own interrupt queue, and `dsp56k::DspBoot`
		 * keeps its `DSP&` private with no accessor, so the constructor's
		 * reference has to be kept here or the bridge has no route back. */
		dsp56k::DSP& m_core;

		/* The boot consumer is the library's and not `g2::Hdi08Bootstrap`. This
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

	// One bridge per slot, host port i to DSP i. The set takes ownership.
	//
	// The adapter has to outlive the set, and that direction is the destructor's
	// and not the callbacks'. `~Hdi08Bridge` uninstalls through the port it was
	// handed, so a set outliving its adapter dereferences a dead port.
	//
	// A second attach on one set is refused. The run gate borrows the pointer
	// `DspSet::programLanded` answers for the whole run, and replacing the
	// bridges would leave that pointer aimed at a destroyed one.
	//
	// A bridged port feeds its boot consumer until a program has landed, and
	// says nothing while it does. The first words a port takes are a count, an
	// address and that many body words; they reach program memory and neither
	// `dsp56k::HDI08` nor any return value, so a driver that skips the handshake
	// has its word absorbed as boot input with nothing to read afterwards. No
	// check here can refuse it: the mistaken word and the firmware's own first
	// word are the same word on the same wire. `DspSet::programLanded` is the
	// report that the handshake completed, and the run gate is what keeps a slot
	// that never completed one from executing.
	void attachHdi08Bridges(Hdi08Adapter& _adapter, DspSet& _set);
}
