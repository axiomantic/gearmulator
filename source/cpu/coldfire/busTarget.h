#pragma once

#include <cstdint>
#include <mcf5407.h>

namespace coldfire
{
	class BusTarget
	{
	public:
		virtual ~BusTarget() = default;
		virtual uint32_t read(uint32_t _offset, int _size, mcf5407_bus_status& _status) = 0;
		virtual void write(uint32_t _offset, int _size, uint32_t _value, mcf5407_bus_status& _status) = 0;
	};
}
