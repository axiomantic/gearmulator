#pragma once

#include "dsp56kEmu/hdi08HostPort.h"

namespace rg2
{
	// Clean-room Host Digital Interface (HDI08) host-side register model
	// arrives through dsp56k::Hdi08HostPort from dsp56kEmu.
	using Hdi08Port = dsp56k::Hdi08HostPort;
	using Hdi08 = Hdi08Port;
}
