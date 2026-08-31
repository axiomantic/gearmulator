/* g2PatchLoad.cpp -- the `.pch2` load path. Task PROTO-11.
 *
 * THE PROVENANCE RECORD FOR THIS CONTAINER IS AT THE HEAD OF g2PatchLoad.h,
 * and it is the part of this task the licence makes matter. In one line: the
 * container is a clean-room derivation from observed bytes and from design
 * sections 15.7 and 15.3; no line of `msg/g2ools` is copied, transliterated or
 * paraphrased here, because no file of it was read.
 *
 * TWO PASSES, AND THE SECOND ONE IS THE ONLY ONE THAT SENDS. A single pass
 * that validated an object and originated it in the same step would leave the
 * device holding the objects before the one that turned out to be malformed --
 * half a patch, which is a patch nobody asked for and which no later refusal
 * can take back. The passes walk the same rule, so the second cannot reach an
 * object the first rejected.
 *
 * NOTHING HERE READS A PAYLOAD. An object is carried whole, header and all,
 * and its bytes are never interpreted. Design section 15.3: "the emulator does
 * not implement the protocol by hand, the firmware implements it; the emulator
 * only carries the bytes."
 *
 * NOTHING HERE ALLOCATES. Each frame BORROWS from the caller's file buffer for
 * the duration of one send, which is exactly the lifetime g2::ProtocolFrame
 * declares for a buffer going into the hub -- valid until the call returns,
 * with the hub copying what it keeps.
 */

#include "g2PatchLoad.h"

#include "../g2Lib/crc16.h"
#include "../g2Lib/internalClient.h"
#include "../g2Lib/transportHub.h"

namespace g2
{
	namespace
	{
		/* THE OBJECT TYPES THIS PROJECT'S OWN SPECIFICATION NAMES, sorted.
		 * Design section 15.7 names 0x21, 0x4D and 0x65 in its text and design
		 * section 18's protocol row names the bit-packed set; this is their
		 * union, and it is the same union `nmg2_tools/synth_pch2.py` states
		 * from the same two sections.
		 *
		 * A TYPE OUTSIDE IT IS REFUSED RATHER THAN CARRIED, and the reason is
		 * not tidiness. The type codes of the real format were DELIBERATELY
		 * left unnamed by the clean-room derivation, because naming them would
		 * have required the reference implementation that was not read. A load
		 * path that forwarded any byte it did not recognise would quietly turn
		 * that open question into traffic on the wire. */
		/* THE THREE CODES ADDED AFTER THE CLEAN-ROOM PASS NAMED THEM, and the
		 * paragraph above is amended rather than left standing: the open
		 * question it protected against is closed for these three and only for
		 * these three. `nmg2-cleanroom-pch2/FINDINGS.txt`, section "THE THREE
		 * UNNAMED TYPE CODES", is the record, and its own provenance statement
		 * is narrower than "clean": every name it gives is FORCED BY A
		 * MEASUREMENT PRINTED BESIDE IT, and where a name was reachable only
		 * by recognition it is not given.
		 *
		 *   0x6F  the patch's optional free-text note. Read out of the file as
		 *         literal text -- 72 of 73 payloads empty, the one non-empty
		 *         payload is an 89-byte three-line credit block.
		 *   0x5A  the per-area module label table. Its declared item count
		 *         equals 0x4A's module count in 73/73 files, in BOTH
		 *         instances, and three rival readings are refuted by that same
		 *         equality.
		 *   0x5B  a 7-character label table. Named AS FAR AS THE BYTES GO: the
		 *         record rule is measured (3-byte header, then 3-byte key and
		 *         7-byte label, at bit offset 2, well-formed rate 1.000 against
		 *         0.400 or below at all 39 other candidates) and what the table
		 *         labels is NOT derived. The FINDINGS record stops there
		 *         deliberately and so does this list.
		 *
		 * NAMING A CODE IS ENOUGH TO CARRY IT AND IS NOT ENOUGH TO READ ONE.
		 * This module interprets no payload of any type, so what it needs from
		 * a derivation is that the code is a real object type of the format --
		 * which a per-file multiplicity holding in 73/73 files establishes on
		 * its own. Every one of the three appears in every real patch, and a
		 * loader that refused them refused all 73.
		 *
		 * WHAT THIS COSTS, STATED. The refusal was a guard against turning an
		 * open question into traffic on the wire, and widening it spends that
		 * guard for three codes. It buys the only thing that makes the load
		 * path reachable at all: with the eight codes below alone, the
		 * computed loadable count over the 73-file corpus is 0. */
		constexpr uint8_t g_objectTypes[] =
			{ 0x21, 0x4A, 0x4D, 0x52, 0x5A, 0x5B, 0x60, 0x62, 0x65, 0x69, 0x6F };

		bool isKnownObjectType(const uint8_t _type) noexcept
		{
			for(const uint8_t known : g_objectTypes)
			{
				if(known == _type)
					return true;
			}
			return false;
		}

		std::size_t objectLength(const uint8_t* const _object) noexcept
		{
			return (static_cast<std::size_t>(_object[1]) << 8) | _object[2];
		}

		/* THE MESSAGE HEADER BYTES, each one measured on the real wire and in
		 * the reference editor's own composer, which agree.
		 *
		 * 0x01 is the command message; 0x28 is the slot request, to which the
		 * slot index is added; 0x53 selects a new patch; 0x37 is the create
		 * command. The three zero bytes that follow are unexplained -- the
		 * reference editor's source marks them unexplained too -- and they are
		 * present in every capture, so they are carried. */
		constexpr uint8_t g_cmd        = 0x01;
		constexpr uint8_t g_slotReq    = 0x28;
		constexpr uint8_t g_newPatch   = 0x53;
		constexpr uint8_t g_create     = 0x37;

		/* The variation count a file carries and the one the wire carries. */
		constexpr uint8_t g_fileVariationCount = 9;
		constexpr uint8_t g_wireVariationCount = 10;

		/* The object types whose variation count differs between file and
		 * wire. 0x65 takes the full tenth-variation transform; 0x4D takes the
		 * count rewrite and one filler byte, which its reader consumed
		 * normally in the measured run. */
		constexpr uint8_t g_typeMorphParameters = 0x65;
		constexpr uint8_t g_typeVariationCount  = 0x4D;

		/* An MSB-first bit reader -- the bit order the protocol uses. */
		class BitReader
		{
		public:
			BitReader(const uint8_t* const _data, const std::size_t _size) noexcept
				: m_data(_data), m_bits(_size * 8) {}

			bool get(const unsigned _count, uint32_t& _value) noexcept
			{
				if(m_pos + _count > m_bits)
					return false;
				uint32_t value = 0;
				for(unsigned i = 0; i < _count; ++i)
				{
					value = (value << 1) | ((m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1);
					++m_pos;
				}
				_value = value;
				return true;
			}

			std::size_t position() const noexcept { return m_pos; }
			std::size_t bits() const noexcept { return m_bits; }

		private:
			const uint8_t* m_data;
			std::size_t    m_bits;
			std::size_t    m_pos = 0;
		};

		/* The counterpart writer. It reports an overflow rather than growing:
		 * the destination is the caller's buffer and nothing here allocates. */
		class BitWriter
		{
		public:
			BitWriter(uint8_t* const _dst, const std::size_t _capacity) noexcept
				: m_dst(_dst), m_capacity(_capacity) {}

			bool put(const unsigned _count, const uint32_t _value) noexcept
			{
				for(int shift = static_cast<int>(_count) - 1; shift >= 0; --shift)
				{
					const std::size_t byte = m_pos >> 3;
					if(byte >= m_capacity)
						return false;
					if((m_pos & 7) == 0)
						m_dst[byte] = 0;
					if((_value >> shift) & 1u)
						m_dst[byte] |= static_cast<uint8_t>(0x80u >> (m_pos & 7));
					++m_pos;
				}
				return true;
			}

			/* The section is padded to a whole byte at its end. */
			std::size_t sizeInBytes() const noexcept { return (m_pos + 7) / 8; }

		private:
			uint8_t*    m_dst;
			std::size_t m_capacity;
			std::size_t m_pos = 0;
		};

		/* The wire payload for a 0x65 object whose file form carries nine
		 * variations: the nine decoded and re-emitted with the count rewritten
		 * to ten, then the LAST variation written again as the tenth.
		 *
		 * Returns false when the payload does not decode exactly through the
		 * layout, and the caller then falls back to the filler form: expanding
		 * a payload the layout does not describe would corrupt fields this
		 * transform does not name. */
		bool morphTenthVariation(const uint8_t* const _payload, const std::size_t _size,
			uint8_t* const _out, const std::size_t _outCapacity, std::size_t& _outSize) noexcept
		{
			/* Per variation: a 4-bit index, three reserved fields of 24, 24 and
			 * 8 bits, an 8-bit morph count and a 4-bit tail -- 72 bits before
			 * any parameter. Nine of them behind a 32-bit header is the
			 * smallest payload the layout can occupy. */
			constexpr unsigned g_variationFixedBits = 4 + 24 + 24 + 8 + 8 + 4;
			constexpr unsigned g_parameterBits      = 2 + 8 + 7 + 4 + 8;
			constexpr unsigned g_variations         = 9;

			if(_size * 8 < 32u + g_variations * g_variationFixedBits)
				return false;

			BitReader reader(_payload, _size);

			uint32_t discardedCount = 0;
			uint32_t morphCount     = 0;
			uint32_t reserved       = 0;

			if(!reader.get(8, discardedCount) || !reader.get(4, morphCount) || !reader.get(20, reserved))
				return false;

			BitWriter writer(_out, _outCapacity);

			if(!writer.put(8, g_wireVariationCount) || !writer.put(4, morphCount) || !writer.put(20, reserved))
				return false;

			/* THE TENTH VARIATION IS THE NINTH WRITTEN AGAIN, so the ninth's
			 * fields are held while the loop runs and emitted once more after
			 * it. Re-reading the payload for the copy would need a second
			 * reader positioned at a bit offset nothing records. */
			uint32_t lastIndex = 0, lastR0 = 0, lastR1 = 0, lastR2 = 0, lastCount = 0, lastTail = 0;
			std::size_t lastParameterBit = 0;

			for(unsigned v = 0; v < g_variations; ++v)
			{
				uint32_t index = 0, r0 = 0, r1 = 0, r2 = 0, count = 0;

				if(!reader.get(4, index) || !reader.get(24, r0) || !reader.get(24, r1)
					|| !reader.get(8, r2) || !reader.get(8, count))
					return false;

				const std::size_t parameterBit = reader.position();

				if(parameterBit + count * g_parameterBits + 4 > reader.bits())
					return false;

				if(!writer.put(4, index) || !writer.put(24, r0) || !writer.put(24, r1)
					|| !writer.put(8, r2) || !writer.put(8, count))
					return false;

				for(uint32_t p = 0; p < count; ++p)
				{
					uint32_t location = 0, module = 0, param = 0, morph = 0, range = 0;
					if(!reader.get(2, location) || !reader.get(8, module) || !reader.get(7, param)
						|| !reader.get(4, morph) || !reader.get(8, range))
						return false;
					if(!writer.put(2, location) || !writer.put(8, module) || !writer.put(7, param)
						|| !writer.put(4, morph) || !writer.put(8, range))
						return false;
				}

				uint32_t tail = 0;
				if(!reader.get(4, tail) || !writer.put(4, tail))
					return false;

				lastIndex = index; lastR0 = r0; lastR1 = r1; lastR2 = r2;
				lastCount = count; lastTail = tail; lastParameterBit = parameterBit;
			}

			/* A PAYLOAD WITH BYTES BEYOND THE LAYOUT IS NOT THIS FORM. The
			 * transform re-emits every bit it read, so a payload holding more
			 * than it read would lose the remainder silently. */
			if(reader.position() != reader.bits())
				return false;

			if(!writer.put(4, lastIndex) || !writer.put(24, lastR0) || !writer.put(24, lastR1)
				|| !writer.put(8, lastR2) || !writer.put(8, lastCount))
				return false;

			BitReader copy(_payload, _size);
			{
				uint32_t skip = 0;
				for(std::size_t bit = 0; bit < lastParameterBit; ++bit)
				{
					if(!copy.get(1, skip))
						return false;
				}
			}

			for(uint32_t p = 0; p < lastCount; ++p)
			{
				uint32_t location = 0, module = 0, param = 0, morph = 0, range = 0;
				if(!copy.get(2, location) || !copy.get(8, module) || !copy.get(7, param)
					|| !copy.get(4, morph) || !copy.get(8, range))
					return false;
				if(!writer.put(2, location) || !writer.put(8, module) || !writer.put(7, param)
					|| !writer.put(4, morph) || !writer.put(8, range))
					return false;
			}

			if(!writer.put(4, lastTail))
				return false;

			_outSize = writer.sizeInBytes();
			return true;
		}

		/* Writes the WIRE payload for one object into `_out` and returns its
		 * length, or returns false when the buffer cannot hold it. */
		bool writeMessagePayload(const uint8_t _type, const uint8_t* const _payload, const std::size_t _size,
			uint8_t* const _out, const std::size_t _outCapacity, std::size_t& _outSize) noexcept
		{
			/* THE PREDICATE IS THE COUNT BYTE ITSELF, because the payloads stay
			 * opaque here. A 0x65 whose first byte is not the file count is not
			 * carrying the variation-count form, and rewriting it would corrupt
			 * a field this difference does not name. */
			if(_size != 0 && _payload[0] == g_fileVariationCount)
			{
				if(_type == g_typeMorphParameters
					&& morphTenthVariation(_payload, _size, _out, _outCapacity, _outSize))
					return true;

				if(_type == g_typeMorphParameters || _type == g_typeVariationCount)
				{
					if(_size + 1 > _outCapacity)
						return false;
					_out[0] = g_wireVariationCount;
					for(std::size_t i = 1; i < _size; ++i)
						_out[i] = _payload[i];
					_out[_size] = 0x00;
					_outSize = _size + 1;
					return true;
				}
			}

			if(_size > _outCapacity)
				return false;
			for(std::size_t i = 0; i < _size; ++i)
				_out[i] = _payload[i];
			_outSize = _size;
			return true;
		}

		/* Validates the container and reports where its object chain runs.
		 * Both the per-object path and the composer walk the same rule, so
		 * neither can accept a file the other refuses. */
		Pch2LoadResult validateContainer(const uint8_t* const _file, const std::size_t _size,
			std::size_t& _bodyStart, std::size_t& _bodyEnd) noexcept
		{
			if(_file == nullptr)
				return Pch2LoadResult::ShortFile;

			std::size_t terminator = 0;
			while(terminator < _size && _file[terminator] != 0)
				++terminator;

			if(terminator == _size)
				return Pch2LoadResult::NoHeaderTerminator;

			const std::size_t binaryHeader = terminator + 1;

			if(_size < binaryHeader + 4)
				return Pch2LoadResult::ShortFile;

			if(crc16File(_file, _size, binaryHeader) != crc16Load(_file + _size - 2))
				return Pch2LoadResult::BadCrc;

			_bodyStart = binaryHeader + 2;
			_bodyEnd   = _size - 2;

			std::size_t offset = _bodyStart;

			while(_bodyEnd - offset >= 3)
			{
				const uint8_t     type   = _file[offset];
				const std::size_t length = objectLength(_file + offset);

				if(!isKnownObjectType(type))
					return Pch2LoadResult::UnknownObjectType;

				if(length > _size)
					return Pch2LoadResult::LengthPastEnd;

				if(offset + 3 + length > _bodyEnd)
					return Pch2LoadResult::TruncatedObject;

				offset += 3 + length;
			}

			if(_bodyEnd - offset == 1)
				return Pch2LoadResult::TruncatedObject;

			return Pch2LoadResult::Loaded;
		}
	}

	const char* pch2LoadResultName(const Pch2LoadResult _result) noexcept
	{
		switch(_result)
		{
		case Pch2LoadResult::Loaded:             return "PCH2-LOADED";
		case Pch2LoadResult::NoHeaderTerminator: return "PCH2-NO-HEADER-TERMINATOR";
		case Pch2LoadResult::ShortFile:          return "PCH2-SHORT-FILE";
		case Pch2LoadResult::BadCrc:             return "PCH2-BAD-CRC";
		case Pch2LoadResult::UnknownObjectType:  return "PCH2-UNKNOWN-OBJECT-TYPE";
		case Pch2LoadResult::LengthPastEnd:      return "PCH2-LENGTH-PAST-END";
		case Pch2LoadResult::TruncatedObject:    return "PCH2-TRUNCATED-OBJECT";
		case Pch2LoadResult::SendRefused:        return "PCH2-SEND-REFUSED";
		case Pch2LoadResult::NameTooLong:        return "PCH2-NAME-TOO-LONG";
		case Pch2LoadResult::BufferTooSmall:     return "PCH2-BUFFER-TOO-SMALL";
		}
		return "PCH2-UNNAMED-RESULT";
	}

	Pch2LoadResult pch2Load(const uint8_t* const _file, const std::size_t _size, InternalClient& _client) noexcept
	{
		if(_file == nullptr)
			return Pch2LoadResult::ShortFile;

		/* The ASCII header runs to the first NUL and the binary header starts
		 * at the byte after it. crc16File takes that offset and does not look
		 * for it -- finding the terminator is a parse of the container, and
		 * the container is parsed here and nowhere else. */
		std::size_t terminator = 0;
		while(terminator < _size && _file[terminator] != 0)
			++terminator;

		if(terminator == _size)
			return Pch2LoadResult::NoHeaderTerminator;

		const std::size_t binaryHeader = terminator + 1;

		/* The smallest container this module will read: a version byte, a type
		 * byte and the 2-byte stored CRC. An object is not required -- a file
		 * that carries none is empty, not malformed. */
		if(_size < binaryHeader + 4)
			return Pch2LoadResult::ShortFile;

		/* THE CHECKSUM IS VERIFIED BEFORE ANYTHING IS WALKED. The structure of
		 * an unverified file means nothing: a length field that a flipped bit
		 * moved is indistinguishable from one that was written that way, and
		 * reporting it as a structural fault would name the wrong defect. */
		if(crc16File(_file, _size, binaryHeader) != crc16Load(_file + _size - 2))
			return Pch2LoadResult::BadCrc;

		const std::size_t bodyStart = binaryHeader + 2;
		const std::size_t bodyEnd   = _size - 2;

		/* PASS 1. Validate every object. Nothing is originated here. */
		std::size_t offset = bodyStart;

		while(bodyEnd - offset >= 3)
		{
			const uint8_t     type   = _file[offset];
			const std::size_t length = objectLength(_file + offset);

			if(!isKnownObjectType(type))
				return Pch2LoadResult::UnknownObjectType;

			/* TWO DIFFERENT FAULTS, AND THE DISCRIMINATOR IS THE WHOLE FILE
			 * RATHER THAN THE REMAINING BODY. A declared length no file of
			 * this size could ever hold is a length field that is wrong --
			 * PCH2-LENGTH-PAST-END. A length that a file of this size could
			 * hold, on an object that runs off the end of this one, is a file
			 * that stops early -- PCH2-TRUNCATED-OBJECT. Collapsing the two
			 * would name a byte-order fault in a length field as a truncated
			 * download. */
			if(length > _size)
				return Pch2LoadResult::LengthPastEnd;

			if(offset + 3 + length > bodyEnd)
				return Pch2LoadResult::TruncatedObject;

			offset += 3 + length;
		}

		/* WHAT REMAINS AFTER THE LAST OBJECT IS NOT AN OBJECT AND IS NOT AN
		 * ERROR. Design section 15.7's second file-against-wire difference
		 * puts two extra bytes, 0x2D 0x00, after the 0x21 chunk in a USB dump,
		 * and an object header is three bytes, so those two can never be one.
		 * They are carried by neither pass: this module originates objects.
		 * A remainder of ONE byte is a different matter -- no rule of the
		 * format puts a single byte there, so the file stopped early. */
		if(bodyEnd - offset == 1)
			return Pch2LoadResult::TruncatedObject;

		/* PASS 2. Originate one frame for each object, in file order. */
		offset = bodyStart;

		while(bodyEnd - offset >= 3)
		{
			const std::size_t length = objectLength(_file + offset);

			if(!_client.send(ProtocolFrame{ _file + offset, 3 + length }))
				return Pch2LoadResult::SendRefused;

			offset += 3 + length;
		}

		return Pch2LoadResult::Loaded;
	}

	std::size_t pch2ComposePatchLoad(const uint8_t* const _file, const std::size_t _size, const char* const _name,
		const uint8_t _slot, uint8_t* const _out, const std::size_t _outCapacity, Pch2LoadResult& _result) noexcept
	{
		std::size_t bodyStart = 0;
		std::size_t bodyEnd   = 0;

		_result = validateContainer(_file, _size, bodyStart, bodyEnd);

		if(_result != Pch2LoadResult::Loaded)
			return 0;

		std::size_t nameLength = 0;
		while(_name != nullptr && _name[nameLength] != 0 && nameLength <= g_entryNameLength)
			++nameLength;

		if(nameLength > g_entryNameLength)
		{
			_result = Pch2LoadResult::NameTooLong;
			return 0;
		}

		/* The entry-name field is the name's characters plus one terminator,
		 * or exactly the field width with none. */
		const std::size_t nameField = nameLength == g_entryNameLength ? g_entryNameLength : nameLength + 1;

		/* Two bytes for the total, seven for the header, the name field, and
		 * two for the CRC -- everything but the chain, which is written and
		 * measured below. */
		std::size_t written = 2;

		if(written + 7 + nameField + 2 > _outCapacity)
		{
			_result = Pch2LoadResult::BufferTooSmall;
			return 0;
		}

		_out[written++] = g_cmd;
		_out[written++] = static_cast<uint8_t>(g_slotReq + _slot);
		_out[written++] = g_newPatch;
		_out[written++] = g_create;
		_out[written++] = 0x00;
		_out[written++] = 0x00;
		_out[written++] = 0x00;

		for(std::size_t i = 0; i < nameLength; ++i)
			_out[written++] = static_cast<uint8_t>(_name[i]);

		if(nameLength != g_entryNameLength)
			_out[written++] = 0x00;

		for(std::size_t offset = bodyStart; bodyEnd - offset >= 3; )
		{
			const uint8_t     type   = _file[offset];
			const std::size_t length = objectLength(_file + offset);

			/* The length field is written after the payload, because the wire
			 * length is the TRANSFORMED payload's and not the file's. */
			if(written + 3 + 2 > _outCapacity)
			{
				_result = Pch2LoadResult::BufferTooSmall;
				return 0;
			}

			const std::size_t headerAt = written;
			written += 3;

			std::size_t payloadSize = 0;

			if(!writeMessagePayload(type, _file + offset + 3, length,
				_out + written, _outCapacity - written - 2, payloadSize))
			{
				_result = Pch2LoadResult::BufferTooSmall;
				return 0;
			}

			/* A payload longer than the 2-byte length field can name cannot be
			 * framed at all, and a truncated field would name a shorter object
			 * than the bytes that follow it. */
			if(payloadSize > 0xFFFFu)
			{
				_result = Pch2LoadResult::BufferTooSmall;
				return 0;
			}

			_out[headerAt]     = type;
			_out[headerAt + 1] = static_cast<uint8_t>(payloadSize >> 8);
			_out[headerAt + 2] = static_cast<uint8_t>(payloadSize & 0xFFu);

			written += payloadSize;
			offset  += 3 + length;
		}

		const std::size_t bodyLength = written - 2;
		const std::size_t total      = written + 2;

		if(total > 0xFFFFu || total > _outCapacity)
		{
			_result = Pch2LoadResult::BufferTooSmall;
			return 0;
		}

		_out[0] = static_cast<uint8_t>(total >> 8);
		_out[1] = static_cast<uint8_t>(total & 0xFFu);

		crc16Store(_out + written, crc16(_out + 2, bodyLength));

		_result = Pch2LoadResult::Loaded;
		return total;
	}

	Pch2LoadResult pch2LoadFramed(const uint8_t* const _file, const std::size_t _size, const char* const _name,
		const uint8_t _slot, InternalClient& _client, uint8_t* const _scratch, const std::size_t _scratchSize) noexcept
	{
		/* THE MESSAGE IS COMPOSED AT OFFSET 2 so that the transfer envelope can
		 * be written around it in place: the client writes the transfer total
		 * ahead of it and the transfer CRC behind it, and no byte is copied a
		 * second time. */
		if(_scratch == nullptr || _scratchSize < 6)
			return Pch2LoadResult::BufferTooSmall;

		Pch2LoadResult result = Pch2LoadResult::Loaded;

		const std::size_t message = pch2ComposePatchLoad(_file, _size, _name, _slot,
			_scratch + 2, _scratchSize - 4, result);

		if(result != Pch2LoadResult::Loaded)
			return result;

		/* THE MESSAGE FRAME IS WHAT LEAVES, AND THE TRANSFER ENVELOPE IS NOT
		 * APPLIED HERE. The delivery the firmware's message worker was
		 * measured ACCEPTING -- D0 = 0 at the switch join -- carried ONE
		 * envelope: the total, the 0x01/0x37 body and the CRC, chunked into
		 * USB packets by the transport. A second envelope around it would put
		 * two bytes the worker reads as the start of the message ahead of the
		 * message. InternalClient::sendTransfer builds the transfer level for
		 * the callers that need it, and the composition above is what it
		 * carries. */
		if(!_client.send(ProtocolFrame{ _scratch + 2, message }))
			return Pch2LoadResult::SendRefused;

		return Pch2LoadResult::Loaded;
	}
}
