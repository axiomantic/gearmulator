// Task BRD-1. The memory decode and the two bus callbacks.
//
// Plan section 13.1, BRD-1. Design sections 5.2.1, 6.4, 17 row 7.24.
// Logbook: AGENTS.md section 2.2.
//
// THIS FILE CARRIES NO ADDRESS AT ALL. The four recorded bases are named
// constants in memoryMap.h and the three unrecorded ones are configuration.
// See the header for the authority of each.
//
// NOTHING HERE ABORTS AND NOTHING HERE USES assert(). A failed access is
// reported through the out-parameter of the callback and it writes one log
// line. The default build is Release and defines NDEBUG, so an assert() would
// be removed and a report built on one could never fire.

#include "memoryMap.h"

#include <ostream>

namespace g2
{
	namespace
	{
		// The order the windows are examined in. It is the order of the Region
		// enumeration and the first match wins.
		constexpr Region g_decodeOrder[] =
		{
			Region::Cs0, Region::Cs1, Region::Cs2, Region::Cs3,
			Region::Cs4, Region::Cs5, Region::Mbar, Region::Sdram,
		};

		size_t indexOf(const Region _region)
		{
			return static_cast<size_t>(_region);
		}

		// An absent window has a size of zero and answers nowhere, so a window
		// a caller left empty never claims address zero. The size comparison
		// alone gives that answer, and an explicit size-of-zero branch beside
		// it would be a branch no input can reach.
		bool contains(const Window& _window, const uint32_t _address)
		{
			if(_address < _window.base)
				return false;
			return (_address - _window.base) < _window.size;
		}

		std::string hex32(const uint32_t _value)
		{
			static const char* digits = "0123456789abcdef";
			std::string result = "0x";
			for(int shift = 28; shift >= 0; shift -= 4)
				result += digits[(_value >> shift) & 0xfu];
			return result;
		}

		const char* statusName(const mcf5307_bus_status _status)
		{
			switch(_status)
			{
			case MCF5307_BUS_OK:           return "OK";
			case MCF5307_BUS_UNMAPPED:     return "UNMAPPED";
			case MCF5307_BUS_SIZE_ILLEGAL: return "SIZE_ILLEGAL";
			case MCF5307_BUS_FAULT:        return "FAULT";
			}
			return "UNKNOWN";
		}

		// The MCF5307 issues 8-bit, 16-bit and 32-bit bus accesses and no
		// other width. One callback pair carries all three.
		bool isLegalWidth(const int _size)
		{
			return _size == 8 || _size == 16 || _size == 32;
		}

		const Window g_absentWindow;
	}

	const char* toString(const Region _region)
	{
		switch(_region)
		{
		case Region::None:  return "none";
		case Region::Cs0:   return "CS0";
		case Region::Cs1:   return "CS1";
		case Region::Cs2:   return "CS2";
		case Region::Cs3:   return "CS3";
		case Region::Cs4:   return "CS4";
		case Region::Cs5:   return "CS5";
		case Region::Mbar:  return "MBAR";
		case Region::Sdram: return "SDRAM";
		}
		return "unknown";
	}

	std::ostream& operator<<(std::ostream& _out, const Region _region)
	{
		return _out << toString(_region);
	}

	MemoryMap::MemoryMap(const MemoryMapConfig& _config) : m_config(_config)
	{
	}

	Region MemoryMap::decode(const uint32_t _address) const
	{
		for(const Region region : g_decodeOrder)
		{
			if(contains(window(region), _address))
				return region;
		}
		return Region::None;
	}

	const Window& MemoryMap::window(const Region _region) const
	{
		switch(_region)
		{
		case Region::Cs0:   return m_config.cs0;
		case Region::Cs1:   return m_config.cs1;
		case Region::Cs2:   return m_config.cs2;
		case Region::Cs3:   return m_config.cs3;
		case Region::Cs4:   return m_config.cs4;
		case Region::Cs5:   return m_config.cs5;
		case Region::Mbar:  return m_config.mbar;
		case Region::Sdram: return m_config.sdram;
		case Region::None:  break;
		}
		return g_absentWindow;
	}

	void MemoryMap::attach(const Region _region, BusTarget* _target)
	{
		m_targets[indexOf(_region)] = _target;
	}

	BusTarget* MemoryMap::target(const Region _region) const
	{
		return m_targets[indexOf(_region)];
	}

	void MemoryMap::logFailure(const mcf5307_bus_status _status, const bool _isWrite,
		const int _size, const uint32_t _address)
	{
		m_log.push_back(std::string("memoryMap: ") + statusName(_status)
			+ (_isWrite ? " write of " : " read of ") + std::to_string(_size)
			+ " bits at " + hex32(_address));
	}

	uint32_t MemoryMap::read(const uint32_t _address, const int _size, mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logFailure(_status, false, _size, _address);
			return 0;
		}

		const Region region = decode(_address);
		BusTarget* const busTarget = region == Region::None ? nullptr : target(region);

		// A window nothing sits in is the same answer as a window that is not
		// decoded at all: no device answers at this address.
		if(!busTarget)
		{
			_status = MCF5307_BUS_UNMAPPED;
			logFailure(_status, false, _size, _address);
			return 0;
		}

		const uint32_t value = busTarget->read(_address - window(region).base, _size, _status);

		if(_status != MCF5307_BUS_OK)
		{
			logFailure(_status, false, _size, _address);
			return 0;
		}

		return value;
	}

	void MemoryMap::write(const uint32_t _address, const int _size, const uint32_t _value,
		mcf5307_bus_status& _status)
	{
		_status = MCF5307_BUS_OK;

		if(!isLegalWidth(_size))
		{
			_status = MCF5307_BUS_SIZE_ILLEGAL;
			logFailure(_status, true, _size, _address);
			return;
		}

		const Region region = decode(_address);
		BusTarget* const busTarget = region == Region::None ? nullptr : target(region);

		if(!busTarget)
		{
			_status = MCF5307_BUS_UNMAPPED;
			logFailure(_status, true, _size, _address);
			return;
		}

		busTarget->write(_address - window(region).base, _size, _value, _status);

		if(_status != MCF5307_BUS_OK)
			logFailure(_status, true, _size, _address);
	}

	uint32_t memoryMapRead(void* _user, const uint32_t _address, const int _size, mcf5307_bus_status* _status)
	{
		mcf5307_bus_status local = MCF5307_BUS_OK;
		const uint32_t value = static_cast<MemoryMap*>(_user)->read(_address, _size, local);
		if(_status)
			*_status = local;
		return value;
	}

	void memoryMapWrite(void* _user, const uint32_t _address, const int _size, const uint32_t _value,
		mcf5307_bus_status* _status)
	{
		mcf5307_bus_status local = MCF5307_BUS_OK;
		static_cast<MemoryMap*>(_user)->write(_address, _size, _value, local);
		if(_status)
			*_status = local;
	}
}
