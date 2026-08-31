/* g2PatchLoad.h -- the `.pch2` load path. Task PROTO-11.
 * Design sections 15.7 and 15.3.
 *
 * THE PLUGIN PARSES THE FILE, THEN DRIVES THE SAME PROTOCOL MESSAGES THAT AN
 * EDITOR WOULD DRIVE. Design section 15.7 states it and this module is the
 * whole of it: there is no second, private load path, so there is one set of
 * bugs. What it originates goes through g2::InternalClient, which design
 * section 15.1 makes a PEER of the usbip endpoint rather than a path through
 * it -- to restore a DAW project the plugin must originate protocol messages
 * and no editor is attached at that moment.
 *
 * HOW THE CONTAINER WAS OBTAINED, because the licence makes it matter.
 *
 * `gearmulator` is GPL-3.0 and this fork's tooling is not. The reference
 * implementation of this container, `msg/g2ools`, is GPL-2.0-or-later
 * (c) 2006-2007 Matt Gerassimoff. NO LINE OF IT IS COPIED, TRANSLITERATED OR
 * PARAPHRASED HERE. No file of that tree was opened, searched, executed or
 * quoted by the author of this file, who confirms reading none of it.
 *
 * The container below is a clean-room derivation from OBSERVED BYTES, made
 * outside every repository of this project and recorded there with its
 * re-runnable observations: the frame encoding, the rule for a frame's length,
 * the range the checksum covers and its parameters. Those are facts about the
 * data. The reference code is a different expression of them and is not used.
 * Design sections 15.7 and 15.3 -- this project's own specification -- state
 * the same framing and the same checksum independently, and this file is
 * written against those two sections.
 *
 * The form of this record follows `nmg2_tools/lzo1x.py`, which is this
 * project's existing clean-room record and states the same distinction for
 * LZO1X.
 *
 * WHAT IS DERIVED AND WHAT IS NOT. The CONTAINER is derived: an ASCII header
 * ended by a NUL, a 2-byte binary header, then objects of
 * [1-byte type][2-byte big-endian length][payload], then a 2-byte stored
 * CRC-16/XMODEM covering the binary header and every object. THE MEANING OF
 * AN OBJECT TYPE IS NOT DERIVED and nothing here reads a payload: this module
 * carries an object's bytes and never interprets one. Design section 15.3 is
 * why that is the right shape and not a shortcut -- "the emulator does not
 * implement the protocol by hand, the firmware implements it; the emulator
 * only carries the bytes."
 *
 * A REFUSED FILE ORIGINATES NOTHING. The whole container is validated before
 * the first frame leaves, so a malformed file cannot leave the device holding
 * half a patch. That is why this module has two passes and not one.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace g2
{
	class InternalClient;

	/* The verdict of one load. Every value except Loaded is a REFUSAL, and
	 * each refusal has a name the corpus states rather than this header:
	 * `MANIFEST.tsv` in the synthesized corpus names the refusal each
	 * malformed file must draw, so a parser that carried the names alone
	 * could never disagree with the corpus. */
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
	 * THE FRAME IS THE OBJECT VERBATIM -- [type][length][payload], which is
	 * the framing g2::ProtocolFrame already carries. Nothing is re-wrapped and
	 * no checksum is recomputed for it: the file's CRC covers the file and the
	 * wire's covers the wire, and design section 15.3 states the two coverages
	 * differ. A frame built here that carried a file checksum would be wrong
	 * on the wire in a way no file test could see.
	 *
	 * Returns Loaded only when every object was validated AND every frame was
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
	 * where the total counts the WHOLE frame including its own two prefix
	 * bytes and the CRC sits DIRECTLY after the body -- there is no pad. A
	 * pad-to-64 rule was an artifact of a synthetic 4096-byte chunking that
	 * never produced a short USB packet; the real wire terminates on the short
	 * last packet, and totals of 865 and 14,664 bytes were measured there.
	 *
	 * THE OBJECT CHAIN CARRIES NO PER-OBJECT CHECKSUM. One CRC covers the whole
	 * body, so a checksum after each object would be bytes the firmware's chain
	 * walk reads as payload.
	 *
	 * TWO PAYLOADS ARE TRANSFORMED ON THE WAY OUT, and both are the variation
	 * count that reads 9 in a file and 10 on the wire. A 0x65 payload that
	 * decodes through the nine-variation bit layout gains a FULL tenth
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
	 * ONE transfer through `_client`.
	 *
	 * `_scratch` holds the message at offset 2 so that InternalClient can write
	 * the transfer envelope around it in place, which is why the buffer must
	 * hold the message plus four bytes. `_scratchSize` below
	 * g_maxPatchLoadMessageBytes + 4 is accepted -- a composition that outgrows
	 * it returns BufferTooSmall. */
	Pch2LoadResult pch2LoadFramed(const uint8_t* _file, std::size_t _size, const char* _name, uint8_t _slot,
		InternalClient& _client, uint8_t* _scratch, std::size_t _scratchSize) noexcept;
}
