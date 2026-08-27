# Source list for the proto track. Owned by the proto track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.

# ----------------- THE LINKAGE GAP THIS FILE WAS CREATED FOR, CLOSED.
#
# This file was EMPTY, and it was empty because no plan block's Files: line
# declared it: PROTO-1 added crc16.cpp, PROTO-3 added internalClient.cpp and
# PROTO-11 added g2PatchLoad.cpp, and each of the three worked around the gap
# by compiling its source DIRECTLY into its own test target. Every one of those
# Check: lines passed while g2Lib carried none of the symbols.
#
# THAT WORKAROUND DOES NOT REACH A REAL CONSUMER. g2JucePlugin LINKS g2Lib
# rather than recompiling its sources, so the moment the plugin track calls
# g2::pch2Load or constructs a g2::InternalClient it fails at the LINK step.
# t0_board_transport is such a consumer -- it links g2Lib for the Board -- so
# the gap is closed here rather than reported a fourth time.
#
# WHAT IS APPENDED AND WHAT IS NOT. crc16.* and internalClient.* are g2Lib
# sources and belong in G2LIB_SOURCES. g2PatchLoad.* is NOT: it lives in
# g2JucePlugin, and appending it here would move a plugin translation unit into
# the library on this file's own authority. It stays where it is, and the one
# target that needs it compiles it directly.
#
# NO OTHER TRACK'S LIST IS WIDENED. This is the proto track's own source list
# and appending to it is the whole reason the file exists.

# ----------------- PROTO-1, the CRC
list(APPEND G2LIB_SOURCES
	crc16.h
	crc16.cpp)

# ----------------- PROTO-3, the internal protocol client
list(APPEND G2LIB_SOURCES
	internalClient.h
	internalClient.cpp)
