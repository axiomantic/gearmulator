// The memory decode and the two bus callbacks.
//
// A failed access is reported through the out-parameter of the callback and it
// writes one log line. Nothing here aborts.

#include "memoryMap.h"

#include <ostream>

namespace rg2
{
	namespace
	{
		// The order the windows are examined in. It is the order of the Region
		// enumeration and the first match wins.
		constexpr Region g_decodeOrder[] = {
			Region::Cs0, Region::Cs1, Region::Cs2, Region::Cs3, Region::Cs4, Region::Cs5, Region::Mbar, Region::Sdram,
		};

		size_t indexOf(const Region _region) { return static_cast<size_t>(_region); }

		// An absent window has a size of zero and answers nowhere, so a window
		// a caller left empty never claims address zero. The size comparison
		// alone gives that answer, and an explicit size-of-zero branch beside
		// it would be a branch no input can reach.
		bool contains(const Window& _window, const uint32_t _address)
		{
			if (_address < _window.base)
				return false;
			return (_address - _window.base) < _window.size;
		}

		std::string hex32(const uint32_t _value)
		{
			static const char* digits = "0123456789abcdef";
			std::string result = "0x";
			for (int shift = 28; shift >= 0; shift -= 4)
				result += digits[(_value >> shift) & 0xfu];
			return result;
		}

		const char* statusName(const cf_bus_status _status)
		{
			switch (_status)
			{
			case CF_BUS_OK:
				return "OK";
			case CF_BUS_UNMAPPED:
				return "UNMAPPED";
			case CF_BUS_SIZE_ILLEGAL:
				return "SIZE_ILLEGAL";
			case CF_BUS_FAULT:
				return "FAULT";
			}
			return "UNKNOWN";
		}

		// The MCF5307 issues 8-bit, 16-bit and 32-bit bus accesses and no
		// other width. One callback pair carries all three.
		bool isLegalWidth(const int _size) { return _size == 8 || _size == 16 || _size == 32; }

		const Window g_absentWindow;
	} // namespace

	const char* toString(const Region _region)
	{
		switch (_region)
		{
		case Region::None:
			return "none";
		case Region::Cs0:
			return "CS0";
		case Region::Cs1:
			return "CS1";
		case Region::Cs2:
			return "CS2";
		case Region::Cs3:
			return "CS3";
		case Region::Cs4:
			return "CS4";
		case Region::Cs5:
			return "CS5";
		case Region::Mbar:
			return "MBAR";
		case Region::Sdram:
			return "SDRAM";
		}
		return "unknown";
	}

	std::ostream& operator<<(std::ostream& _out, const Region _region) { return _out << toString(_region); }

	void MemoryMap::initRegions()
	{
		const auto addr = reinterpret_cast<uintptr_t>(m_regionStorage);
		const auto aligned = (addr + 63u) & ~uintptr_t(63u);
		m_regions = reinterpret_cast<RegionEntry*>(aligned);
	}

	MemoryMap::MemoryMap(const MemoryMapConfig& _config) : m_config(_config)
	{
		initRegions();
		for (size_t i = 0; i < 9; ++i)
			m_regions[i] = {};

		m_regions[indexOf(Region::Cs0)].window = _config.cs0;
		m_regions[indexOf(Region::Cs1)].window = _config.cs1;
		m_regions[indexOf(Region::Cs2)].window = _config.cs2;
		m_regions[indexOf(Region::Cs3)].window = _config.cs3;
		m_regions[indexOf(Region::Cs4)].window = _config.cs4;
		m_regions[indexOf(Region::Cs5)].window = _config.cs5;
		m_regions[indexOf(Region::Mbar)].window = _config.mbar;
		m_regions[indexOf(Region::Sdram)].window = _config.sdram;
	}

	MemoryMap::MemoryMap(const MemoryMap& _other) : m_config(_other.m_config), m_log(_other.m_log)
	{
		initRegions();
		for (size_t i = 0; i < 9; ++i)
			m_regions[i] = _other.m_regions[i];
	}

	MemoryMap& MemoryMap::operator=(const MemoryMap& _other)
	{
		if (this != &_other)
		{
			m_config = _other.m_config;
			m_log = _other.m_log;
			initRegions();
			for (size_t i = 0; i < 9; ++i)
				m_regions[i] = _other.m_regions[i];
		}
		return *this;
	}

	MemoryMap::MemoryMap(MemoryMap&& _other) noexcept : m_config(_other.m_config), m_log(std::move(_other.m_log))
	{
		initRegions();
		for (size_t i = 0; i < 9; ++i)
			m_regions[i] = _other.m_regions[i];
	}

	MemoryMap& MemoryMap::operator=(MemoryMap&& _other) noexcept
	{
		if (this != &_other)
		{
			m_config = _other.m_config;
			m_log = std::move(_other.m_log);
			initRegions();
			for (size_t i = 0; i < 9; ++i)
				m_regions[i] = _other.m_regions[i];
		}
		return *this;
	}

	Region MemoryMap::decode(const uint32_t _address) const
	{
		if (contains(m_regions[indexOf(Region::Sdram)].window, _address))
			return Region::Sdram;

		for (const Region region : g_decodeOrder)
		{
			if (region == Region::Sdram)
				continue;
			if (contains(m_regions[indexOf(region)].window, _address))
				return region;
		}
		return Region::None;
	}

	const Window& MemoryMap::window(const Region _region) const { return m_regions[indexOf(_region)].window; }

	void MemoryMap::attach(const Region _region, BusTarget* const _target)
	{
		m_regions[indexOf(_region)].target = _target;
	}

	BusTarget* MemoryMap::target(const Region _region) const { return m_regions[indexOf(_region)].target; }

	void MemoryMap::logFailure(const cf_bus_status _status, const bool _isWrite, const int _size,
							   const uint32_t _address)
	{
		m_log.push_back(std::string("memoryMap: ") + statusName(_status) + (_isWrite ? " write of " : " read of ") +
						std::to_string(_size) + " bits at " + hex32(_address));
	}

	uint32_t MemoryMap::read(const uint32_t _address, const int _size, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if (!isLegalWidth(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			logFailure(_status, false, _size, _address);
			return 0;
		}

		const Region region = decode(_address);
		const size_t idx = indexOf(region);
		BusTarget* const busTarget = m_regions[idx].target;

		// A window nothing sits in is the same answer as a window that is not
		// decoded at all: no device answers at this address.
		if (!busTarget)
		{
			_status = CF_BUS_UNMAPPED;
			logFailure(_status, false, _size, _address);
			return 0;
		}

		const uint32_t value = busTarget->read(_address - m_regions[idx].window.base, _size, _status);

		if (_status != CF_BUS_OK)
		{
			logFailure(_status, false, _size, _address);
			return 0;
		}

		return value;
	}

	void MemoryMap::write(const uint32_t _address, const int _size, const uint32_t _value, cf_bus_status& _status)
	{
		_status = CF_BUS_OK;

		if (!isLegalWidth(_size))
		{
			_status = CF_BUS_SIZE_ILLEGAL;
			logFailure(_status, true, _size, _address);
			return;
		}

		const Region region = decode(_address);
		const size_t idx = indexOf(region);
		BusTarget* const busTarget = m_regions[idx].target;

		if (!busTarget)
		{
			_status = CF_BUS_UNMAPPED;
			logFailure(_status, true, _size, _address);
			return;
		}

		busTarget->write(_address - m_regions[idx].window.base, _size, _value, _status);

		if (_status != CF_BUS_OK)
			logFailure(_status, true, _size, _address);
	}

	uint32_t memoryMapRead(void* _user, const uint32_t _address, const int _size, cf_bus_status* _status)
	{
		cf_bus_status local = CF_BUS_OK;
		const uint32_t value = static_cast<MemoryMap*>(_user)->read(_address, _size, local);
		if (_status)
			*_status = local;
		return value;
	}

	void memoryMapWrite(void* _user, const uint32_t _address, const int _size, const uint32_t _value,
						cf_bus_status* _status)
	{
		cf_bus_status local = CF_BUS_OK;
		static_cast<MemoryMap*>(_user)->write(_address, _size, _value, local);
		if (_status)
			*_status = local;
	}
} // namespace rg2
