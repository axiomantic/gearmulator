// Task BRD-6. The C++ firmware extractor. See firmwareExtract.h for what this
// file is, for the licence statement, and for the stored-section report.

#include "firmwareExtract.h"

#include <cstdio>

namespace g2
{
	namespace
	{
		// ------------------------------------------------------------------
		// The message texts.
		//
		// EVERY ONE OF THESE IS WORD FOR WORD THE TEXT THE PYTHON ORACLE
		// WRITES, and BRD-6's test compares whole messages. A message that
		// only nearly matched would turn a parity assertion into a
		// near-parity assertion, which is not what design section 20.2 asks
		// for.

		std::string decimal(const uint64_t _value)
		{
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(_value));
			return buffer;
		}

		// Uppercase, and at least `_digits` wide. Python's `0x{v:08X}`.
		std::string hex(const uint32_t _value, const int _digits)
		{
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%0*X", _digits, _value);
			return buffer;
		}

		// Lowercase and two digits for each byte. Python's `bytes.hex()`.
		std::string hexBytes(const uint8_t* _data, const size_t _size)
		{
			static const char* digits = "0123456789abcdef";
			std::string result;
			for(size_t i = 0; i < _size; ++i)
			{
				result += digits[(_data[i] >> 4) & 0x0Fu];
				result += digits[_data[i] & 0x0Fu];
			}
			return result;
		}

		uint16_t readBe16(const std::vector<uint8_t>& _image, const size_t _offset)
		{
			return uint16_t((uint16_t(_image[_offset]) << 8) | uint16_t(_image[_offset + 1]));
		}

		uint32_t readBe32(const std::vector<uint8_t>& _image, const size_t _offset)
		{
			return (uint32_t(_image[_offset + 0]) << 24)
				| (uint32_t(_image[_offset + 1]) << 16)
				| (uint32_t(_image[_offset + 2]) << 8)
				| uint32_t(_image[_offset + 3]);
		}

		// ------------------------------------------------------------------
		// The LZO1X wire format.
		//
		// A long-distance match is above this base, and the same instruction
		// form with a distance of 0 is the END of the stream.
		constexpr uint32_t g_longDistanceBase = 0x4000u;

		// A short match that follows a literal run reaches back at least this
		// far.
		constexpr uint32_t g_headMatchBase = 0x0801u;

		// A bounds-checked cursor over the compressed bytes. Every read that
		// could run off the end reports through `error` and leaves `failed`
		// set, so the decoder below tests one flag rather than every read.
		class Reader
		{
		public:
			Reader(const uint8_t* _data, const size_t _size) : m_data(_data), m_size(_size) {}

			bool failed() const { return m_failed; }
			const std::string& error() const { return m_error; }
			size_t position() const { return m_position; }

			// One opcode byte. The end of the input at this point is a stream
			// with NO END MARKER, which is a different fault from a truncated
			// read: every well-formed stream stops at an end marker, so there
			// is always one more opcode to read.
			uint8_t instruction()
			{
				if(m_position >= m_size)
				{
					fail("LZO-MISSING-END-MARKER: input ended at offset " + decimal(m_position)
						+ " with no end marker");
					return 0;
				}
				return m_data[m_position++];
			}

			uint8_t byte()
			{
				if(!require(1))
					return 0;
				return m_data[m_position++];
			}

			// The two-byte forms hold a LITTLE-ENDIAN word, and the low two
			// bits of it are the trailing-literal count.
			uint32_t word()
			{
				if(!require(2))
					return 0;
				const uint32_t low = m_data[m_position];
				const uint32_t high = m_data[m_position + 1];
				m_position += 2;
				return low | (high << 8);
			}

			bool copyLiterals(std::vector<uint8_t>& _out, const size_t _count)
			{
				if(!require(_count))
					return false;
				_out.insert(_out.end(), m_data + m_position, m_data + m_position + _count);
				m_position += _count;
				return true;
			}

			// The special case of the FIRST byte of the stream: a value above
			// 17 means the stream opens with a literal run.
			bool atStartOfLiteralRun() const
			{
				return m_position < m_size && m_data[m_position] > 17u;
			}

			// A length that did not fit in the opcode. The chain reads bytes
			// while they are 0 and adds 255 for each; the first byte that is
			// not 0 is added to the base and stops the chain.
			uint32_t extendedLength(const uint32_t _base)
			{
				uint32_t extra = 0;
				for(;;)
				{
					const uint8_t value = byte();
					if(m_failed)
						return 0;
					if(value != 0)
						return _base + extra + value;
					extra += 255u;
				}
			}

		private:
			bool require(const size_t _count)
			{
				if(m_position + _count <= m_size)
					return true;

				fail("LZO-TRUNCATED-INPUT: " + decimal(_count) + (_count == 1 ? " byte" : " bytes")
					+ " needed at offset " + decimal(m_position) + ", "
					+ decimal(m_size - m_position) + " available");
				return false;
			}

			void fail(const std::string& _error)
			{
				if(m_failed)
					return;
				m_failed = true;
				m_error = _error;
			}

			const uint8_t* m_data;
			size_t m_size;
			size_t m_position = 0;
			bool m_failed = false;
			std::string m_error;
		};

		// Copies `_length` bytes that start `_distance` bytes back in `_out`.
		//
		// A DISTANCE SHORTER THAN THE LENGTH REPEATS the last `_distance`
		// bytes, which is what a byte-at-a-time copy produces. That is a
		// property of the format and not an accident: the compressor uses it
		// to encode a long run of one value, so the copy below must not be a
		// memcpy of a fixed source range.
		bool copyMatch(std::vector<uint8_t>& _out, const uint32_t _distance, const uint32_t _length,
			std::string& _error)
		{
			if(_distance > _out.size())
			{
				_error = "LZO-DISTANCE-BEFORE-START: distance " + decimal(_distance)
					+ " is more than the " + decimal(_out.size()) + " bytes written";
				return false;
			}

			const size_t start = _out.size() - _distance;
			for(uint32_t i = 0; i < _length; ++i)
				_out.push_back(_out[start + i]);
			return true;
		}

		// The positions the format distinguishes. A literal run and a run of
		// trailing literals are followed by DIFFERENT READINGS OF THE SAME
		// OPCODE VALUE, so the position is part of the state.
		enum class State
		{
			Start,
			Instruction,
			AfterLiterals,
			BeforeMatch,
			Match,
			Trailing,
		};
	}

	uint32_t containerChecksum(const uint8_t* _data, const size_t _size)
	{
		uint32_t sum = 0;
		for(size_t i = 0; i < _size; ++i)
			sum += _data[i];
		return ~sum;
	}

	uint32_t containerChecksum(const std::vector<uint8_t>& _data)
	{
		return containerChecksum(_data.data(), _data.size());
	}

	bool lzo1xDecompress(const uint8_t* _src, const size_t _size, std::vector<uint8_t>& _out,
		std::string& _error)
	{
		_out.clear();
		_error.clear();

		Reader reader(_src, _size);

		State state = State::Start;
		uint8_t opcode = 0;
		uint32_t trailing = 0;

		for(;;)
		{
			switch(state)
			{
			case State::Start:
				if(reader.atStartOfLiteralRun())
				{
					const uint32_t count = uint32_t(reader.instruction()) - 17u;
					if(!reader.copyLiterals(_out, count))
						break;
					// A run of fewer than four bytes is a run of TRAILING
					// literals, and a match follows it.
					state = count >= 4u ? State::AfterLiterals : State::BeforeMatch;
				}
				else
				{
					state = State::Instruction;
				}
				break;

			case State::Instruction:
				{
					opcode = reader.instruction();
					if(reader.failed())
						break;
					if(opcode >= 16u)
					{
						state = State::Match;
						break;
					}
					const uint32_t count = opcode != 0 ? uint32_t(opcode) : reader.extendedLength(15u);
					if(reader.failed())
						break;
					if(!reader.copyLiterals(_out, count + 3u))
						break;
					state = State::AfterLiterals;
				}
				break;

			case State::AfterLiterals:
				{
					opcode = reader.instruction();
					if(reader.failed())
						break;
					if(opcode >= 16u)
					{
						state = State::Match;
						break;
					}
					const uint32_t next = reader.byte();
					if(reader.failed())
						break;
					const uint32_t distance = g_headMatchBase + (uint32_t(opcode) >> 2) + (next << 2);
					if(!copyMatch(_out, distance, 3u, _error))
					{
						_out.clear();
						return false;
					}
					trailing = uint32_t(opcode) & 3u;
					state = State::Trailing;
				}
				break;

			case State::BeforeMatch:
				opcode = reader.instruction();
				if(reader.failed())
					break;
				state = State::Match;
				break;

			case State::Match:
				{
					uint32_t length = 0;
					uint32_t distance = 0;

					if(opcode >= 64u)
					{
						length = (uint32_t(opcode) >> 5) + 1u;
						const uint32_t next = reader.byte();
						if(reader.failed())
							break;
						distance = 1u + ((uint32_t(opcode) >> 2) & 7u) + (next << 3);
						trailing = uint32_t(opcode) & 3u;
					}
					else if(opcode >= 32u)
					{
						length = uint32_t(opcode) & 31u;
						if(length == 0u)
							length = reader.extendedLength(31u);
						if(reader.failed())
							break;
						length += 2u;
						const uint32_t value = reader.word();
						if(reader.failed())
							break;
						distance = 1u + (value >> 2);
						trailing = value & 3u;
					}
					else if(opcode >= 16u)
					{
						length = uint32_t(opcode) & 7u;
						if(length == 0u)
							length = reader.extendedLength(7u);
						if(reader.failed())
							break;
						length += 2u;
						const uint32_t value = reader.word();
						if(reader.failed())
							break;
						const uint32_t back = ((uint32_t(opcode) & 8u) << 11) + (value >> 2);
						// The END MARKER. This is the one exit that is not a
						// failure, and it is why the loop has no other.
						if(back == 0u)
							return true;
						distance = g_longDistanceBase + back;
						trailing = value & 3u;
					}
					else
					{
						length = 2u;
						const uint32_t next = reader.byte();
						if(reader.failed())
							break;
						distance = 1u + (uint32_t(opcode) >> 2) + (next << 2);
						trailing = uint32_t(opcode) & 3u;
					}

					if(!copyMatch(_out, distance, length, _error))
					{
						_out.clear();
						return false;
					}
					state = State::Trailing;
				}
				break;

			case State::Trailing:
				if(trailing == 0u)
				{
					state = State::Instruction;
					break;
				}
				if(!reader.copyLiterals(_out, trailing))
					break;
				state = State::BeforeMatch;
				break;
			}

			if(reader.failed())
			{
				_out.clear();
				_error = reader.error();
				return false;
			}
		}
	}

	std::string versionText(const uint16_t _version)
	{
		char buffer[16];
		std::snprintf(buffer, sizeof(buffer), "%u.%02u", unsigned(_version / 100u), unsigned(_version % 100u));
		return buffer;
	}

	bool parseHeader(const std::vector<uint8_t>& _image, Container& _container, std::string& _error)
	{
		_container = Container();
		_error.clear();

		if(_image.size() < g_containerHeaderSize)
		{
			_error = "CONTAINER-TRUNCATED-HEADER: " + decimal(g_containerHeaderSize)
				+ " bytes needed, " + decimal(_image.size()) + " available";
			return false;
		}

		const uint16_t version = readBe16(_image, 0x00);
		const uint16_t secondWord = readBe16(_image, 0x02);
		const uint32_t unresolved = readBe32(_image, 0x04);

		if(secondWord != g_containerSecondWord)
		{
			_error = "CONTAINER-BAD-SECOND-WORD: 0x" + hex(secondWord, 4) + " at offset 0x02, expected 0x"
				+ hex(g_containerSecondWord, 4);
			return false;
		}

		const uint32_t count = readBe32(_image, 0x10);

		// THE PRODUCT IS 64-BIT ON PURPOSE. A section count near 2^32 times the
		// 0x2C stride overflows 32 bits, and an overflowed product compares
		// SMALLER than the bytes available -- so a wrapped count would pass the
		// check it exists to fail, and the walk would then read past the image.
		const uint64_t needed = uint64_t(count) * uint64_t(g_containerEntryStride);
		const uint64_t available = uint64_t(_image.size()) - uint64_t(g_containerHeaderSize);

		if(needed > available)
		{
			_error = "CONTAINER-TRUNCATED-SECTION-TABLE: " + decimal(count) + " entries need "
				+ decimal(needed) + " bytes at offset 0x" + hex(g_containerHeaderSize, 2) + ", "
				+ decimal(available) + " available";
			return false;
		}

		_container.version = version;
		_container.secondWord = secondWord;
		_container.unresolved = unresolved;
		_container.sections.reserve(count);

		for(uint32_t index = 0; index < count; ++index)
		{
			const size_t offset = size_t(g_containerHeaderSize) + size_t(index) * size_t(g_containerEntryStride);

			// The tag is four bytes and it must be ASCII, so no byte of it may
			// have its top bit set.
			for(size_t i = 0; i < 4; ++i)
			{
				if(_image[offset + i] < 0x80u)
					continue;

				_container = Container();
				_error = "CONTAINER-BAD-SECTION-TAG: entry " + decimal(index) + " holds "
					+ hexBytes(&_image[offset], 4) + ", which is not ASCII";
				return false;
			}

			ContainerSection section;
			section.tag.assign(reinterpret_cast<const char*>(&_image[offset]), 4);
			section.fileOffset = readBe32(_image, offset + 0x04);
			section.uncompressedLength = readBe32(_image, offset + 0x08);
			section.loadAddress = readBe32(_image, offset + 0x0C);
			section.plainChecksum = readBe32(_image, offset + 0x10);
			section.compressedLength = readBe32(_image, offset + 0x14);
			section.compressedChecksum = readBe32(_image, offset + 0x18);
			section.reserved = readBe32(_image, offset + 0x1C);

			// The last 12 bytes of the entry carry no meaning this project
			// knows and they are NOT read. The stride steps over them.
			_container.sections.push_back(section);
		}

		return true;
	}

	namespace
	{
		// `_length` bytes at the section's file offset. The check is on the
		// bytes PRESENT, so an offset or a length the image cannot satisfy is a
		// named failure and never a short read.
		bool sliceSection(const std::vector<uint8_t>& _image, const ContainerSection& _section,
			const uint32_t _length, std::vector<uint8_t>& _slice, std::string& _error)
		{
			const uint64_t size = uint64_t(_image.size());
			const uint64_t offset = uint64_t(_section.fileOffset);
			const uint64_t available = offset < size ? size - offset : 0u;

			if(uint64_t(_length) > available)
			{
				_error = "CONTAINER-SECTION-OUT-OF-RANGE: section " + _section.tag + " needs "
					+ decimal(_length) + " bytes at offset 0x" + hex(_section.fileOffset, 2) + ", "
					+ decimal(available) + " available";
				return false;
			}

			_slice.assign(_image.begin() + size_t(offset), _image.begin() + size_t(offset) + _length);
			return true;
		}
	}

	bool loadSection(const std::vector<uint8_t>& _image, const ContainerSection& _section,
		std::vector<uint8_t>& _plain, std::string& _error)
	{
		_plain.clear();
		_error.clear();

		if(_section.isStored())
		{
			// THE STORED PATH. No stream, no compressed checksum. See the
			// report at the top of firmwareExtract.h.
			if(!sliceSection(_image, _section, _section.uncompressedLength, _plain, _error))
			{
				_plain.clear();
				return false;
			}
		}
		else
		{
			std::vector<uint8_t> stream;
			if(!sliceSection(_image, _section, _section.compressedLength, stream, _error))
				return false;

			const uint32_t computed = containerChecksum(stream);
			if(computed != _section.compressedChecksum)
			{
				_error = "CONTAINER-COMPRESSED-CHECKSUM: section " + _section.tag + " stored 0x"
					+ hex(_section.compressedChecksum, 8) + ", computed 0x" + hex(computed, 8);
				return false;
			}

			if(!lzo1xDecompress(stream.data(), stream.size(), _plain, _error))
			{
				_plain.clear();
				return false;
			}

			if(_plain.size() != size_t(_section.uncompressedLength))
			{
				_error = "CONTAINER-LENGTH-MISMATCH: section " + _section.tag + " declared "
					+ decimal(_section.uncompressedLength) + " bytes, the stream produced "
					+ decimal(_plain.size());
				_plain.clear();
				return false;
			}

			// THE CONSUMED-LENGTH IDENTITY. See firmwareExtract.h for why this
			// is one more decompression and not a comparison.
			std::vector<uint8_t> shorter;
			std::string ignored;
			if(!stream.empty()
				&& lzo1xDecompress(stream.data(), stream.size() - 1, shorter, ignored))
			{
				_error = "CONTAINER-TRAILING-BYTES: section " + _section.tag + " declared "
					+ decimal(_section.compressedLength) + " compressed bytes, the stream ended "
					+ "before the last of them";
				_plain.clear();
				return false;
			}
		}

		const uint32_t computed = containerChecksum(_plain);
		if(computed != _section.plainChecksum)
		{
			_error = "CONTAINER-PLAIN-CHECKSUM: section " + _section.tag + " stored 0x"
				+ hex(_section.plainChecksum, 8) + ", computed 0x" + hex(computed, 8);
			_plain.clear();
			return false;
		}

		return true;
	}

	bool loadSections(const std::vector<uint8_t>& _image,
		std::vector<std::pair<ContainerSection, std::vector<uint8_t>>>& _loaded, std::string& _error)
	{
		_loaded.clear();

		Container container;
		if(!parseHeader(_image, container, _error))
			return false;

		for(const ContainerSection& section : container.sections)
		{
			std::vector<uint8_t> plain;
			if(!loadSection(_image, section, plain, _error))
			{
				_loaded.clear();
				return false;
			}
			_loaded.push_back(std::make_pair(section, plain));
		}

		return true;
	}
}
