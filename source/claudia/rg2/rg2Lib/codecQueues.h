#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "frame.h"

namespace rg2
{
	// Host input -> chain head.
	class CodecSource
	{
	public:
		explicit CodecSource(size_t capacityFrames);

		// Drops frame and returns false when full.
		bool push(const Frame& frame) noexcept;

		// Returns front frame, or silence frame if empty.
		const Frame& front() const noexcept;

		// Consumes front frame and increments starve count if empty.
		void pop() noexcept;

		size_t size() const noexcept;
		size_t capacity() const noexcept;

		uint64_t overflowFrames() const noexcept;
		uint64_t starvedFrames() const noexcept;

	private:
		std::vector<Frame> m_ring;
		size_t m_readIndex = 0;
		size_t m_count = 0;
		uint64_t m_overflow = 0;
		uint64_t m_starved = 0;
		Frame m_silence{};
	};

	// Chain tail -> host output.
	class CodecSink
	{
	public:
		explicit CodecSink(size_t capacityFrames);

		// Refuses and returns false when full.
		bool push(const Frame& frame) noexcept;

		// Takes up to `frames` frames, padding with silence if underflowing.
		size_t pull(Frame* out, size_t frames) noexcept;

		size_t size() const noexcept;
		size_t capacity() const noexcept;

		uint64_t droppedFrames() const noexcept;
		uint64_t underflowFrames() const noexcept;

	private:
		std::vector<Frame> m_ring;
		size_t m_readIndex = 0;
		size_t m_count = 0;
		uint64_t m_dropped = 0;
		uint64_t m_underflow = 0;
	};

	static_assert(std::is_same_v<std::remove_extent_t<decltype(Frame::slot)>, int32_t>,
				  "A queued frame is Q23 integer storage. No floating-point type may "
				  "enter the determinism boundary.");
} // namespace rg2
