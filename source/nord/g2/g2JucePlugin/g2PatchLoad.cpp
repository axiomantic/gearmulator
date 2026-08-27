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
}
