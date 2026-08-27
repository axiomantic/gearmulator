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
		SendRefused
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
}
