/* g2PatchLoad.cpp -- the `.pch2` load path.
 *
 * Two passes, and the second one is the only one that sends. A single pass
 * that validated an object and originated it in the same step would leave the
 * device holding the objects before the one that turned out to be malformed --
 * half a patch, which no later refusal can take back. The passes walk the same
 * rule, so the second cannot reach an object the first rejected.
 *
 * Nothing here reads a payload. An object is carried whole, header and all,
 * and its bytes are never interpreted: the firmware implements the protocol,
 * the emulator only carries the bytes.
 *
 * Nothing here allocates. Each frame borrows from the caller's file buffer for
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
		/* The object types this project's own specification names, sorted. A
		 * type outside the list is refused rather than carried: the type codes of
		 * the real format were left unnamed, and a load path that forwarded any
		 * byte it did not recognise would quietly turn that open question into
		 * traffic on the wire.
		 *
		 *   0x6F  the patch's optional free-text note, read as literal text.
		 *   0x5A  the per-area module label table. Its declared item count equals
		 *         0x4A's module count, in both instances.
		 *   0x5B  a 7-character label table: a 3-byte header, then a 3-byte key
		 *         and a 7-byte label, at bit offset 2. What the table labels is
		 *         not derived.
		 *
		 * Naming a code is enough to carry it and is not enough to read one. This
		 * module interprets no payload of any type, so what it needs is that the
		 * code is a real object type of the format. */
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

		/* The checksum is verified before anything is walked. The structure of
		 * an unverified file means nothing: a length field that a flipped bit
		 * moved is indistinguishable from one that was written that way, and
		 * reporting it as a structural fault would name the wrong defect. */
		if(crc16File(_file, _size, binaryHeader) != crc16Load(_file + _size - 2))
			return Pch2LoadResult::BadCrc;

		const std::size_t bodyStart = binaryHeader + 2;
		const std::size_t bodyEnd   = _size - 2;

		/* Pass 1. Validate every object. Nothing is originated here. */
		std::size_t offset = bodyStart;

		while(bodyEnd - offset >= 3)
		{
			const uint8_t     type   = _file[offset];
			const std::size_t length = objectLength(_file + offset);

			if(!isKnownObjectType(type))
				return Pch2LoadResult::UnknownObjectType;

			/* Two different faults, and the discriminator is the whole file
			 * rather than the remaining body. A declared length no file of
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

		/* What remains after the last object is not an object and is not an
		 * error. A USB dump puts two extra bytes, 0x2D 0x00, after the 0x21
		 * chunk, and an object header is three bytes, so those two can never be
		 * one. They are carried by neither pass: this module originates
		 * objects. A remainder of one byte is a different matter -- no rule of
		 * the format puts a single byte there, so the file stopped early. */
		if(bodyEnd - offset == 1)
			return Pch2LoadResult::TruncatedObject;

		/* Pass 2. Originate one frame for each object, in file order. */
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
}
