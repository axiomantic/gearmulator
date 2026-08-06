// board.cpp — BRD-21
//
// Implements g2::Board. The MCF5307 link is behind G2_LINK_MCF5307; when that
// option is OFF the mcf5307_* symbols are absent and every call-site guards on
// m_mcfCtx being non-null as the only safe path.

#include "board.h"
#include <cassert>
#include <cstring>

#ifdef G2_LINK_MCF5307
#  include <mcf5307.h>
#endif

namespace g2
{

// ---------------------------------------------------------------------------
// helpers — big-endian RAM access
// ---------------------------------------------------------------------------

namespace
{
	uint32_t ramReadBe(const uint8_t* base, uint32_t offset, int size) noexcept
	{
		uint32_t v = 0;
		for (int i = 0; i < size; ++i)
			v = (v << 8u) | base[offset + i];
		return v;
	}

	void ramWriteBe(uint8_t* base, uint32_t offset, int size, uint32_t val) noexcept
	{
		for (int i = size - 1; i >= 0; --i)
		{
			base[offset + i] = static_cast<uint8_t>(val & 0xFFu);
			val >>= 8u;
		}
	}
} // anonymous namespace

// ---------------------------------------------------------------------------
// static bus callbacks — forwarded to the Board instance
// ---------------------------------------------------------------------------

uint32_t Board::staticRead(void* _user, uint32_t _addr, int _size,
                            mcf5307_bus_status* _status)
{
	return static_cast<Board*>(_user)->handleRead(_addr, _size, _status);
}

void Board::staticWrite(void* _user, uint32_t _addr, int _size, uint32_t _val,
                         mcf5307_bus_status* _status)
{
	static_cast<Board*>(_user)->handleWrite(_addr, _size, _val, _status);
}

void Board::staticIack(void* _user, int _level, uint8_t _vector)
{
	static_cast<Board*>(_user)->handleIack(_level, _vector);
}

// ---------------------------------------------------------------------------
// constructor
// ---------------------------------------------------------------------------

Board::Board(uint32_t _cs0Base, uint32_t _cs0Size,
             uint32_t _cs2Base, uint32_t _cs2Size)
    : m_flash(_cs0Base, _cs0Size, _cs2Base, _cs2Size)
    , m_hdi08()
{
	// 4 MB internal SDRAM — conservative; nothing in unit tests exercises it.
	constexpr size_t kRamBytes = 4u * 1024u * 1024u;
	m_ram.resize(kRamBytes, 0u);

#ifdef G2_LINK_MCF5307
	mcf5307_runtime_init();
	m_mcfCtx = mcf5307_create(this, staticRead, staticWrite, staticIack);
	if (m_mcfCtx)
	{
		// Reset vector is read from flash CS0 bytes 0-7 (big-endian SP, PC).
		// If no ROM is loaded the flash returns 0xFF bytes — the CPU will
		// fault on the first fetch; m_faulted is set in handleRead so that
		// subsequent calls to runMcu are a safe no-op.
		const uint32_t sp = m_flash.read32(_cs0Base);
		const uint32_t pc = m_flash.read32(_cs0Base + 4u);
		mcf5307_reset(m_mcfCtx, sp, pc);
	}
#endif
}

// ---------------------------------------------------------------------------
// destructor
// ---------------------------------------------------------------------------

Board::~Board()
{
#ifdef G2_LINK_MCF5307
	if (m_mcfCtx)
	{
		mcf5307_destroy(m_mcfCtx);
		m_mcfCtx = nullptr;
	}
	if (m_usbCtx)
	{
		isp1181_destroy(m_usbCtx);
		m_usbCtx = nullptr;
	}
#endif
}

// ---------------------------------------------------------------------------
// BRD-21: runMcu
//
// Step the MCF5307 for up to _cycles bus cycles and return the number of
// cycles actually consumed.
//
// Safe no-op when:
//   • G2_LINK_MCF5307 is OFF (mcf5307_* symbols absent)
//   • m_mcfCtx is null (create failed)
//   • m_faulted is true (bus error encountered earlier)
// ---------------------------------------------------------------------------

uint32_t Board::runMcu(uint32_t _cycles)
{
#ifdef G2_LINK_MCF5307
	if (m_mcfCtx && !m_faulted)
		return mcf5307_exec(m_mcfCtx, _cycles);
#endif
	(void)_cycles;
	return 0u;
}

// ---------------------------------------------------------------------------
// faulted
// ---------------------------------------------------------------------------

bool Board::faulted() const noexcept
{
	return m_faulted;
}

// ---------------------------------------------------------------------------
// tickSofIfDue
// ---------------------------------------------------------------------------

void Board::tickSofIfDue(uint64_t _frameIndex)
{
	// TODO(SCH-xx): drive isp1181_tick when the USB SOF scheduler is wired up.
	(void)_frameIndex;
}

// ---------------------------------------------------------------------------
// state size / save / load
// ---------------------------------------------------------------------------

size_t Board::stateSize() const noexcept
{
	size_t total = m_ram.size();
#ifdef G2_LINK_MCF5307
	if (m_mcfCtx)
		total += mcf5307_state_size();
	if (m_usbCtx)
		total += isp1181_state_size();
#endif
	return total;
}

void Board::stateSave(uint8_t* _dst) const
{
	std::memcpy(_dst, m_ram.data(), m_ram.size());
	_dst += m_ram.size();
#ifdef G2_LINK_MCF5307
	if (m_mcfCtx)
	{
		mcf5307_state_save(m_mcfCtx, _dst);
		_dst += mcf5307_state_size();
	}
	if (m_usbCtx)
	{
		isp1181_state_save(m_usbCtx, _dst);
	}
#endif
}

void Board::stateLoad(const uint8_t* _src)
{
	std::memcpy(m_ram.data(), _src, m_ram.size());
	_src += m_ram.size();
#ifdef G2_LINK_MCF5307
	if (m_mcfCtx)
	{
		// The mcf5307 C API takes a non-const void* — safe to cast as the
		// callee only reads and the storage outlives the call.
		mcf5307_state_load(m_mcfCtx, const_cast<uint8_t*>(_src));
		_src += mcf5307_state_size();
	}
	if (m_usbCtx)
	{
		isp1181_state_load(m_usbCtx, const_cast<uint8_t*>(_src));
	}
#endif
}

// ---------------------------------------------------------------------------
// bus read handler (called from staticRead)
// ---------------------------------------------------------------------------

uint32_t Board::handleRead(uint32_t _addr, int _size,
                            mcf5307_bus_status* _status)
{
	if (_status)
		*_status = MCF5307_BUS_OK;

	// --- Flash CS0 / CS2 ---
	if (m_flash.containsCs0(_addr) || m_flash.containsCs2(_addr))
	{
		switch (_size)
		{
		case 1: return m_flash.read8(_addr);
		case 2: return m_flash.read16(_addr);
		case 4: return m_flash.read32(_addr);
		default:
			if (_status) *_status = MCF5307_BUS_SIZE_ILLEGAL;
			return 0u;
		}
	}

	// --- Internal SRAM (base 0x00000000) ---
	const uint32_t ramTop = static_cast<uint32_t>(m_ram.size());
	if (_addr < ramTop && (_addr + static_cast<uint32_t>(_size)) <= ramTop)
		return ramReadBe(m_ram.data(), _addr, _size);

	// --- Unrecognised address ---
	if (_status)
		*_status = MCF5307_BUS_UNMAPPED;
	m_faulted = true;
	return 0u;
}

// ---------------------------------------------------------------------------
// bus write handler (called from staticWrite)
// ---------------------------------------------------------------------------

void Board::handleWrite(uint32_t _addr, int _size, uint32_t _val,
                         mcf5307_bus_status* _status)
{
	if (_status)
		*_status = MCF5307_BUS_OK;

	// Flash is read-only from the CPU; silently ignore (Flash::write* already
	// logs but does not modify state).
	if (m_flash.containsCs0(_addr) || m_flash.containsCs2(_addr))
	{
		switch (_size)
		{
		case 1: m_flash.write8(_addr, static_cast<uint8_t>(_val)); break;
		case 2: m_flash.write16(_addr, static_cast<uint16_t>(_val)); break;
		case 4: m_flash.write32(_addr, _val); break;
		default:
			if (_status) *_status = MCF5307_BUS_SIZE_ILLEGAL;
			break;
		}
		return;
	}

	// Internal SRAM
	const uint32_t ramTop = static_cast<uint32_t>(m_ram.size());
	if (_addr < ramTop && (_addr + static_cast<uint32_t>(_size)) <= ramTop)
	{
		ramWriteBe(m_ram.data(), _addr, _size, _val);
		return;
	}

	// Unrecognised
	if (_status)
		*_status = MCF5307_BUS_UNMAPPED;
	m_faulted = true;
}

// ---------------------------------------------------------------------------
// interrupt-acknowledge handler (called from staticIack)
// ---------------------------------------------------------------------------

void Board::handleIack(int _level, uint8_t _vector)
{
	m_intCtrl.handleIack(_level, _vector);
}

} // namespace g2
