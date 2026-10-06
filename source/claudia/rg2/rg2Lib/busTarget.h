#pragma once

#include <coldfire.h>
#include <cstdint>

namespace rg2
{
    class BusTarget
    {
    public:
        virtual ~BusTarget() = default;
        virtual uint32_t read(uint32_t _offset, int _size, cf_bus_status& _status) = 0;
        virtual void write(uint32_t _offset, int _size, uint32_t _value, cf_bus_status& _status) = 0;
    };
} // namespace rg2

namespace coldfire
{
    using BusTarget = rg2::BusTarget;
}
