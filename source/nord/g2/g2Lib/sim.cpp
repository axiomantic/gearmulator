// The SIM registers.
//
// Sources: the MCF5307 ColdFire Integrated Microprocessor User's Manual
// (Motorola, 1998), and this project's own firmware measurements.
//
//   * Table B-1 ends at MBAR+$3D4, which is why sim.h models one kilobyte.
//   * A write to a reserved address has no effect. A read of a write-only
//     register returns zeros and a write to a read-only register has no
//     effect.
//   * "All UART module registers must be accessed as bytes." MBAR+$1D0 is
//     UIPCR1, a UART module register, and it is the one access-width
//     restriction this model enforces.
//   * MBAR+$1D0 is UIPCR1 on read and UACR1 on write. Both are 8 bits.
//   * Port A data at MBAR+$248 bit 9 reads LOW and the bit is an INPUT.
//     is_expanded() at 0x30037F64 reads it. The manual gives PADAT reset
//     $0000 with no strap; the strap is ours.
//   * MBAR+$1D0 bit 0 reads zero, so the machine is not an Engine.
//     detect_model() at 0x30050864 reads it. This is why the reset value
//     modelled here is $0E and not the manual's $0F.
//   * The G2 wires six chip selects, CS0 to CS5. The part carries eight.
//     CS6 and CS7 are not modelled, because no G2 signal reaches them.
//
// The chip-select rows below do not match the 1998 manual and must not be
// corrected to it. The manual documents an earlier silicon mask with one
// shared CSBAR; the G2 carries the newer layout. The boot loader,
// BOOT_128_Loader.bin, writes CSAR3 at MBAR+$0A4 = $1300, CSMR3 at
// MBAR+$0A8 = $00000001 and CSCR3 at MBAR+$0AE = $0540. Under the 1998
// layout those are reserved holes and the writes would have no effect. The
// USB driver reaches the ISP1181 at 0x13000000, which CSAR3 = $1300 gives
// directly under the newer layout and which the 1998 layout would put at
// 0x13600000. The three measured offsets are each exactly $24 above the CS0
// register of the same kind, so the family repeats every twelve bytes as
// CSARn, CSMRn, CSCRn. CSARn is a 16-bit value, CSMRn a 32-bit value, CSCRn
// a 16-bit value.
//
// The differences, so that no later reader has to rediscover them:
//
//   $098  modelled CSAR2, 16 bits. 1998 manual: CSBAR, 8 bits, shared by
//         CS2 to CS7.
//   $09C  modelled CSMR2, 32 bits. 1998 manual: CSMR2, 16 bits.
//   $0A4  modelled CSAR3, 16 bits. 1998 manual: reserved, 32 bits.
//   $0A8  modelled CSMR3, 32 bits. 1998 manual: CSMR3 at $0AA, 16 bits.
//   $0B0  modelled CSAR4, 16 bits. 1998 manual: reserved, 32 bits.
//   $0B4  modelled CSMR4, 32 bits. 1998 manual: CSMR4 at $0B6, 16 bits.
//   $0BC  modelled CSAR5, 16 bits. 1998 manual: reserved, 32 bits.
//   $0C0  modelled CSMR5, 32 bits. 1998 manual: CSMR5 at $0C2, 16 bits.
//
// The manual states no access-width rule for the chip-select, DRAM
// controller, timer or parallel-port registers. This model accepts every
// width on them, which is a property of the model and not a claim about the
// part.
//
// MBAR+$1D0 belongs to the UART block, which another file owns. This model
// answers that one offset because the firmware reads it as a model strap.

#include "sim.h"

namespace g2
{
	namespace
	{
		enum class Access { ReadWrite, ReadOnly };

		struct RegisterSpec
		{
			uint32_t offset;
			uint32_t widthBytes;
			Access access;
			bool byteAccessOnly;
			uint32_t resetValue;
			uint32_t strapBits;     // bits a board strap holds, which a write cannot change
			const char* name;
		};

		constexpr RegisterSpec g_registers[] =
		{
			// The six chip selects the G2 wires, one CSARn, CSMRn and CSCRn
			// each. These rows do not match the 1998 manual; the head of this
			// file has the evidence and the list of differences.
			{0x080, 2, Access::ReadWrite, false, 0,      0, "CSAR0"},
			{0x084, 4, Access::ReadWrite, false, 0,      0, "CSMR0"},
			{0x08a, 2, Access::ReadWrite, false, 0,      0, "CSCR0"},
			{0x08c, 2, Access::ReadWrite, false, 0,      0, "CSAR1"},
			{0x090, 4, Access::ReadWrite, false, 0,      0, "CSMR1"},
			{0x096, 2, Access::ReadWrite, false, 0,      0, "CSCR1"},
			{0x098, 2, Access::ReadWrite, false, 0,      0, "CSAR2"},
			{0x09c, 4, Access::ReadWrite, false, 0,      0, "CSMR2"},
			{0x0a2, 2, Access::ReadWrite, false, 0,      0, "CSCR2"},
			{0x0a4, 2, Access::ReadWrite, false, 0,      0, "CSAR3"},
			{0x0a8, 4, Access::ReadWrite, false, 0,      0, "CSMR3"},
			{0x0ae, 2, Access::ReadWrite, false, 0,      0, "CSCR3"},
			{0x0b0, 2, Access::ReadWrite, false, 0,      0, "CSAR4"},
			{0x0b4, 4, Access::ReadWrite, false, 0,      0, "CSMR4"},
			{0x0ba, 2, Access::ReadWrite, false, 0,      0, "CSCR4"},
			{0x0bc, 2, Access::ReadWrite, false, 0,      0, "CSAR5"},
			{0x0c0, 4, Access::ReadWrite, false, 0,      0, "CSMR5"},
			{0x0c6, 2, Access::ReadWrite, false, 0,      0, "CSCR5"},

			// The DRAM controller.
			{0x100, 2, Access::ReadWrite, false, 0,      0, "DCR"},
			{0x108, 4, Access::ReadWrite, false, 0,      0, "DACR0"},
			{0x10c, 4, Access::ReadWrite, false, 0,      0, "DMR0"},
			{0x110, 4, Access::ReadWrite, false, 0,      0, "DACR1"},
			{0x114, 4, Access::ReadWrite, false, 0,      0, "DMR1"},

			// The two timers.
			{0x140, 2, Access::ReadWrite, false, 0x0000, 0, "TMR1"},
			{0x144, 2, Access::ReadWrite, false, 0xffff, 0, "TRR1"},
			{0x148, 2, Access::ReadOnly,  false, 0x0000, 0, "TCR1"},
			{0x14c, 2, Access::ReadWrite, false, 0x0000, 0, "TCN1"},
			{0x151, 1, Access::ReadWrite, false, 0x00,   0, "TER1"},
			{0x180, 2, Access::ReadWrite, false, 0x0000, 0, "TMR2"},
			{0x184, 2, Access::ReadWrite, false, 0xffff, 0, "TRR2"},
			{0x188, 2, Access::ReadOnly,  false, 0x0000, 0, "TCR2"},
			{0x18c, 2, Access::ReadWrite, false, 0x0000, 0, "TCN2"},
			{0x191, 1, Access::ReadWrite, false, 0x00,   0, "TER2"},

			// The model strap. The manual gives UIPCR reset $0F; bit 0 is
			// fixed at zero on this machine, so it presents $0E. A read
			// returns UIPCR and a write reaches UACR, so a write never changes
			// what a read returns.
			//
			// strapBits is $00 here on purpose: Access::ReadOnly already holds
			// every bit of the register, so naming bit 0 again would add no
			// protection.
			{0x1d0, 1, Access::ReadOnly,  true,  0x0e,   0x00, "UIPCR"},

			// The parallel port. Port A bit 9 is an input strap.
			{0x244, 2, Access::ReadWrite, false, 0x0000, 0x0000, "PADDR"},
			{0x248, 2, Access::ReadWrite, false, 0x0000, 0x0200, "PADAT"},
		};

		// The bits a write cannot reach, for one byte of one register. A
		// read-only register holds every bit. A read/write register holds only
		// the bits a board strap drives.
		//
		// It is constexpr so that the static_assert block below can guard it.
		// A static_assert is not removed by NDEBUG, so the invariant is
		// checked in the Release build the project ships.
		constexpr uint8_t protectByte(const Access _access, const uint32_t _strapBits,
			const uint32_t _widthBytes, const uint32_t _byte)
		{
			const int shift = int(8 * (_widthBytes - 1 - _byte));
			const uint8_t base = _access == Access::ReadOnly ? 0xffu : 0x00u;
			return uint8_t(base | uint8_t((_strapBits >> shift) & 0xffu));
		}

		// The invariant: whatever the access kind, every bit a board strap
		// holds is a bit a write cannot reach. Narrowing either arm breaks the
		// build here instead of going quiet. No runtime case can be red on the
		// choice of arm, because Access::ReadOnly already sets every bit of
		// the mask and the two forms are the same function.
		static_assert((protectByte(Access::ReadWrite, 0x0200u, 2, 0) & 0x02u) == 0x02u,
			"a strap named on a read/write row must reach the write-protect mask");
		static_assert((protectByte(Access::ReadOnly, 0x0200u, 2, 0) & 0x02u) == 0x02u,
			"a strap named on a read-only row must reach the write-protect mask");
		static_assert(protectByte(Access::ReadWrite, 0x0200u, 2, 0) == 0x02u,
			"a read/write row protects the bits a strap holds and no other bit");
		static_assert(protectByte(Access::ReadWrite, 0x0200u, 2, 1) == 0x00u,
			"a byte of a read/write row that carries no strap bit stays fully writable");
		static_assert(protectByte(Access::ReadOnly, 0x0000u, 1, 0) == 0xffu,
			"a read-only row protects every bit");

		const RegisterSpec* find(const uint32_t _offset)
		{
			for(const RegisterSpec& spec : g_registers)
			{
				if(_offset >= spec.offset && (_offset - spec.offset) < spec.widthBytes)
					return &spec;
			}
			return nullptr;
		}

		bool isLegalWidth(const int _size)
		{
			return _size == 8 || _size == 16 || _size == 32;
		}

		std::string hex32(const uint32_t _value)
		{
			static const char* digits = "0123456789abcdef";
			std::string result = "0x";
			for(int shift = 28; shift >= 0; shift -= 4)
				result += digits[(_value >> shift) & 0xfu];
			return result;
		}
	}

	Sim::Sim()
	{
		// Every byte starts write-protected: a write to a reserved address has
		// no effect.
		for(uint8_t& protect : m_writeProtect)
			protect = 0xffu;

		for(const RegisterSpec& spec : g_registers)
		{
			// The part is big-endian, so byte 0 of a register holds its most
			// significant eight bits.
			for(uint32_t byte = 0; byte < spec.widthBytes; ++byte)
			{
				const int shift = int(8 * (spec.widthBytes - 1 - byte));
				const uint32_t index = spec.offset + byte;

				m_space[index] = uint8_t((spec.resetValue >> shift) & 0xffu);

				m_writeProtect[index] = protectByte(spec.access, spec.strapBits, spec.widthBytes, byte);
			}
		}
	}

	void Sim::logLine(const char* _reason, const bool _isWrite, const int _size, const uint32_t _offset)
	{
		m_log.push_back(std::string("sim: ") + _reason
			+ (_isWrite ? " write of " : " read of ") + std::to_string(_size)
			+ " bits at offset " + hex32(_offset));
	}

	uint32_t Sim::read(const uint32_t _offset, const int _size, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0;
		}

		const RegisterSpec* const spec = find(_offset);

		if(!spec)
			logLine("UNMODELLED", false, _size, _offset);
		else if(spec->byteAccessOnly && _size != 8)
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", false, _size, _offset);
			return 0;
		}

		const uint32_t bytes = uint32_t(_size) / 8u;
		uint32_t value = 0;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			value <<= 8;
			if(index < g_simSpaceSize)
				value |= m_space[index];
		}

		return value;
	}

	void Sim::write(const uint32_t _offset, const int _size, const uint32_t _value, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		const RegisterSpec* const spec = find(_offset);

		if(!spec)
			logLine("UNMODELLED", true, _size, _offset);
		else if(spec->byteAccessOnly && _size != 8)
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logLine("SIZE_ILLEGAL", true, _size, _offset);
			return;
		}

		const uint32_t bytes = uint32_t(_size) / 8u;

		for(uint32_t byte = 0; byte < bytes; ++byte)
		{
			const uint32_t index = _offset + byte;
			if(index >= g_simSpaceSize)
				continue;

			const int shift = int(8 * (bytes - 1 - byte));
			const uint8_t incoming = uint8_t((_value >> shift) & 0xffu);
			const uint8_t protect = m_writeProtect[index];

			m_space[index] = uint8_t((m_space[index] & protect) | (incoming & ~protect));
		}
	}
}
