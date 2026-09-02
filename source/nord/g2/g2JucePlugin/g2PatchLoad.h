/* g2PatchLoad.h -- the `.pch2` load path.
 *
 * The plugin parses the file, then drives the same protocol messages that an
 * editor would drive. What it originates goes through g2::InternalClient, a
 * peer of the usbip endpoint rather than a path through it -- to restore a DAW
 * project the plugin must originate protocol messages and no editor is
 * attached at that moment.
 *
 * The container is an ASCII header ended by a NUL, a 2-byte binary header,
 * then objects of [1-byte type][2-byte big-endian length][payload], then a
 * 2-byte stored CRC-16/XMODEM covering the binary header and every object.
 * The meaning of an object type is not derived and nothing here reads a
 * payload: this module carries an object's bytes and never interprets one.
 *
 * A file refused by validation originates nothing. The whole container is
 * checked before the first frame leaves, so a malformed file cannot leave the
 * device holding half a patch. That is why this module has two passes and not
 * one.
 *
 * SendRefused is the one refusal that does not carry that guarantee. Pass 2
 * stops at the first frame the hub declines, and the frames already accepted
 * stay queued and are delivered at the next quantum boundary, so a container
 * with more objects than the hub's queue depth leaves a prefix on the device.
 * The hub is not asked in advance because it cannot answer: the depth free
 * inside one quantum is its queue depth minus what the previous drain still
 * has borrowed, so a capacity check would return a number that is already
 * stale by the last object. Pass-1 validation is what the malformed-file case
 * needs, and it is exact.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace g2
{
	class InternalClient;

	/* The verdict of one load. Every value except Loaded is a refusal, and each
	 * refusal has a name `MANIFEST.tsv` in the synthesized corpus states rather
	 * than this header. */
	enum class Pch2LoadResult
	{
		Loaded,
		NoHeaderTerminator,
		ShortFile,
		BadCrc,
		UnknownObjectType,
		LengthPastEnd,
		TruncatedObject,
		SendRefused,
		NameTooLong,
		BufferTooSmall
	};

	/* The name, as `MANIFEST.tsv` spells it. Never null. */
	const char* pch2LoadResultName(Pch2LoadResult _result) noexcept;

	/* Parses `_file` whole, then originates one protocol frame for each object
	 * in it, in file order, through `_client`.
	 *
	 * The frame is the object verbatim -- [type][length][payload], which is the
	 * framing g2::ProtocolFrame already carries. Nothing is re-wrapped and no
	 * checksum is recomputed for it: the file's CRC covers the file and the
	 * wire's covers the wire, and the two coverages differ. A frame built here
	 * that carried a file checksum would be wrong on the wire in a way no file
	 * test could see.
	 *
	 * Returns Loaded only when every object was validated and every frame was
	 * accepted. SendRefused reports the hub's own refusal -- a frame above the
	 * hub's maxFrameBytes, or a queue already full -- and it is returned
	 * rather than swallowed, because a swallowed refusal tells the plugin that
	 * a patch message it never delivered was sent. */
	Pch2LoadResult pch2Load(const uint8_t* _file, std::size_t _size, InternalClient& _client) noexcept;

	/* The 16-character entry-name field the patch-load header carries. A name
	 * of fewer than 16 characters is followed by one 0x00 terminator; a name of
	 * exactly 16 carries none. A longer name is refused rather than truncated:
	 * a short field slides every byte of the object chain behind it. */
	constexpr std::size_t g_entryNameLength = 16;

	/* The largest patch-load message this composer will build.
	 *
	 * A `.pch2` grows on its way to the wire -- a 0x65 object gains a whole
	 * tenth variation and a 0x4D gains one byte -- so a buffer sized to the
	 * file is too small. The measured corpus file composes to 3,818 bytes and
	 * the largest object the synthesized corpus drives is 65,469 bytes; this
	 * ceiling holds both with room for the expansion, and a composition that
	 * would exceed it returns BufferTooSmall rather than writing past the end. */
	constexpr std::size_t g_maxPatchLoadMessageBytes = 128u * 1024u;

	/* Composes the patch-load MESSAGE frame for `_file` into `_out`.
	 *
	 * The frame is
	 *
	 *   [2-byte BE total][0x01][0x28+slot][0x53][0x37][0x00 0x00 0x00]
	 *   [entry name][object chain][2-byte BE CRC-16/XMODEM over the body]
	 *
	 * where the total counts the whole frame including its own two prefix
	 * bytes and the CRC sits directly after the body -- there is no pad. The
	 * wire terminates on the short last packet, and totals of 865 and 14,664
	 * bytes were measured there.
	 *
	 * The object chain carries no per-object checksum. One CRC covers the whole
	 * body, so a checksum after each object would be bytes the firmware's chain
	 * walk reads as payload.
	 *
	 * The payloads transformed on the way out carry the variation count, which
	 * reads 9 in a file and 10 on the wire. A 0x65 payload that
	 * decodes through the nine-variation bit layout gains a full tenth
	 * variation -- a copy of the last, 297 bytes for the measured file. A
	 * single filler byte does not work: the firmware's reader walks a
	 * continuous bit stream whose per-variation footprint is an 8-bit index,
	 * MorphCount 7-bit fields, an 8-bit parameter count and that many 29-bit
	 * parameters, so a short tenth leaves it reading the FOLLOWING chunk's
	 * bytes as a parameter count and overshooting the section by 37 bytes. A
	 * 0x4D payload, and any 0x65 the layout does not fully describe, take the
	 * count rewrite and one zero filler byte.
	 *
	 * Returns the number of bytes written and sets `_result` to Loaded, or
	 * returns 0 with `_result` naming the refusal. Nothing is allocated: the
	 * whole composition is written into the caller's buffer. */
	std::size_t pch2ComposePatchLoad(const uint8_t* _file, std::size_t _size, const char* _name, uint8_t _slot,
		uint8_t* _out, std::size_t _outCapacity, Pch2LoadResult& _result) noexcept;

	/* Validates `_file`, composes its patch-load message and originates it as
	 * one transfer through `_client`.
	 *
	 * `_scratch` holds the message at offset 2 so that InternalClient can write
	 * the transfer envelope around it in place, which is why the buffer must
	 * hold the message plus four bytes. `_scratchSize` below
	 * g_maxPatchLoadMessageBytes + 4 is accepted -- a composition that outgrows
	 * it returns BufferTooSmall. */
	Pch2LoadResult pch2LoadFramed(const uint8_t* _file, std::size_t _size, const char* _name, uint8_t _slot,
		InternalClient& _client, uint8_t* _scratch, std::size_t _scratchSize) noexcept;
}
