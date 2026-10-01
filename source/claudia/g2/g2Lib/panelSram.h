// The panel board's static RAM, and the CS4 window that reaches it.
//
// The firmware's boot calibration accepts a panel at rest and runs on into
// code that reads this bank. A Board that wires the converter without mapping
// the bank faults there, so the two belong together: `panelAdcConfig()` in
// board.h is the panel's electrical description and this file is the storage
// the same panel carries.

#pragma once

#include <cstdint>
#include <vector>

#include "memoryMap.h"

namespace g2
{
	/* The bank the firmware reads. Its base and size are the part's own; the
	 * image the artifacts carry sits at g_panelSramImageBase inside it. */
	constexpr uint32_t g_panelSramBase = 0x20000000u;
	constexpr uint32_t g_panelSramSize = 0x00800000u;

	// Where SRAM_20000800.bin belongs, and what it is called.
	constexpr uint32_t    g_panelSramImageBase = 0x20000800u;
	constexpr const char* g_panelSramImageName = "SRAM_20000800.bin";

	/* The window a caller must give CS4 for the bank to be reachable.
	 *
	 * No authority records a base or a size for CS4 -- memoryMap.h says so --
	 * so this is a modelling choice and not a datasheet figure. It has to span
	 * from the CS4 base the rest of the tree uses up to, and not into, the
	 * SDRAM at 0x30000000, because the firmware writes at 0x14000000 early in
	 * its boot and a bus error there ends the run in the vector handler, while
	 * the bank itself is at 0x20000000. One window must therefore carry both,
	 * and everything between them is the panel and latch hole. */
	constexpr Window g_panelSramCs4Window{0x14000000u, 0x1C000000u};

	/* Storage for the bank, and the answer for everything else the window
	 * above covers.
	 *
	 * The hole below the bank reads zero and absorbs a write, which is what
	 * the boot needs of it. The one exception is the CS5 latches: the window
	 * spans their base, and the decode reaches CS4 before CS5, so a bank that
	 * answered that sub-range itself would take the panel identifier strap off
	 * the machine and the firmware would report the wrong model. The latch
	 * model the Board already owns is handed that sub-range back.
	 *
	 * The constructor takes the map rather than the two pieces so that no
	 * caller can wire the bank and forget the latches; the same reasoning put
	 * the converter in BoardConfig without a default. */
	class PanelSram final : public BusTarget
	{
	public:
		explicit PanelSram(MemoryMap& _memory);

		uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) override;
		void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) override;

		/* Copies an image into the bank at an absolute address. False when the
		 * image is empty, does not start inside the bank, or does not fit in
		 * it, so a caller that names the wrong address or reads no file is told
		 * rather than silently mapping nothing. */
		bool place(uint32_t _absolute, const std::vector<uint8_t>& _image);

		// Writes that landed in the bank, and writes that landed in the hole.
		uint64_t bankWrites() const { return m_bankWrites; }
		uint64_t holeWrites() const { return m_holeWrites; }

	private:
		// True when the absolute address belongs to the latches, and then
		// `_latchOffset` is that address inside the CS5 window.
		bool isLatch(uint32_t _absolute, uint32_t& _latchOffset) const;

		std::vector<uint8_t> m_bank;
		BusTarget*           m_latches = nullptr;
		Window               m_latchWindow;
		uint64_t             m_bankWrites = 0;
		uint64_t             m_holeWrites = 0;
	};
}
