#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dsp56k
{
	class DSP;
	class Memory;
	class Peripherals56311;
}

namespace g2
{
	enum class Status
	{
		Ok = 0,
		Error,
		BadSize
	};

	class DspSet
	{
	public:
		static constexpr size_t kMaxDsps = 8;

		explicit DspSet(size_t _count = kMaxDsps);
		~DspSet();

		DspSet(const DspSet&) = delete;
		DspSet& operator=(const DspSet&) = delete;
		DspSet(DspSet&&) = delete;
		DspSet& operator=(DspSet&&) = delete;

		size_t count() const noexcept { return m_dsps.size(); }

		dsp56k::DSP& getDsp(size_t _index);
		const dsp56k::DSP& getDsp(size_t _index) const;

		dsp56k::Peripherals56311& getPeripherals(size_t _index);
		const dsp56k::Peripherals56311& getPeripherals(size_t _index) const;

		dsp56k::Memory& getMemory(size_t _index);
		const dsp56k::Memory& getMemory(size_t _index) const;

		size_t stateSize() const noexcept;
		Status stateSave(void* _dst, size_t _size) const;
		Status stateLoad(const void* _src, size_t _size);

	private:
		struct Instance;
		std::vector<std::unique_ptr<Instance>> m_dsps;
	};
}
