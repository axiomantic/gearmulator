/* g2PatchLoad.h -- the `.pch2` load path.
 *
 * The plugin parses the file, then drives the same protocol messages that an
 * editor would drive. This module is the whole of it: there is no second,
 * private load path, so there is one set of bugs. What it originates goes
 * through g2::InternalClient, a peer of the usbip endpoint rather than a path
 * through it -- to restore a DAW project the plugin must originate protocol
 * messages and no editor is attached at that moment.
 *
 * The container is an ASCII header ended by a NUL, a 2-byte binary header,
 * then objects of [1-byte type][2-byte big-endian length][payload], then a
 * 2-byte stored CRC-16/XMODEM covering the binary header and every object.
 * The meaning of an object type is not derived and nothing here reads a
 * payload: this module carries an object's bytes and never interprets one.
 * The firmware implements the protocol; the emulator only carries the bytes.
 *
 * A refused file originates nothing. The whole container is validated before
 * the first frame leaves, so a malformed file cannot leave the device holding
 * half a patch. That is why this module has two passes and not one.
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
		SendRefused
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
}
