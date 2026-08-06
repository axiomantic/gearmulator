#include "dspSet.h"

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "dsp56kEmu/peripherals56311.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace g2
{
	struct DspSet::Instance
	{
		dsp56k::DefaultMemoryValidator memValidator;
		dsp56k::Memory memory;
		dsp56k::Peripherals56311 peripherals;
		dsp56k::PeripheralsNop nopPeripherals;
		std::unique_ptr<dsp56k::DSP> dsp;

		Instance()
			: memory(memValidator)
		{
			dsp = std::make_unique<dsp56k::DSP>(memory, &peripherals, &nopPeripherals);
		}
	};

	DspSet::DspSet(size_t _count)
	{
		const size_t n = std::min(_count, kMaxDsps);
		m_dsps.reserve(n);
		for (size_t i = 0; i < n; ++i)
		{
			m_dsps.push_back(std::make_unique<Instance>());
		}
	}

	DspSet::~DspSet() = default;

	dsp56k::DSP& DspSet::getDsp(size_t _index)
	{
		return *m_dsps.at(_index)->dsp;
	}

	const dsp56k::DSP& DspSet::getDsp(size_t _index) const
	{
		return *m_dsps.at(_index)->dsp;
	}

	dsp56k::Peripherals56311& DspSet::getPeripherals(size_t _index)
	{
		return m_dsps.at(_index)->peripherals;
	}

	const dsp56k::Peripherals56311& DspSet::getPeripherals(size_t _index) const
	{
		return m_dsps.at(_index)->peripherals;
	}

	dsp56k::Memory& DspSet::getMemory(size_t _index)
	{
		return m_dsps.at(_index)->memory;
	}

	const dsp56k::Memory& DspSet::getMemory(size_t _index) const
	{
		return m_dsps.at(_index)->memory;
	}

	struct PerDspSnapshot
	{
		uint64_t instructionCounter;
		uint64_t cycles;
	};

	size_t DspSet::stateSize() const noexcept
	{
		return m_dsps.size() * sizeof(PerDspSnapshot);
	}

	Status DspSet::stateSave(void* _dst, size_t _size) const
	{
		const size_t required = stateSize();
		if (_size < required)
			return Status::BadSize;

		auto* out = static_cast<PerDspSnapshot*>(_dst);
		for (size_t i = 0; i < m_dsps.size(); ++i)
		{
			const auto& dsp = *m_dsps[i]->dsp;
			out[i].instructionCounter = dsp.getInstructionCounter();
			out[i].cycles = dsp.getCycles();
		}
		return Status::Ok;
	}

	Status DspSet::stateLoad(const void* _src, size_t _size)
	{
		const size_t required = stateSize();
		if (_size < required)
			return Status::BadSize;

		return Status::Ok;
	}
}
