# Source list for the proto track. Owned by the proto track.
#
# Append every source name this track adds under source/claudia/rg2/rg2Lib/ to
# RG2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

# A test target that compiles a source directly does not prove rg2Lib carries
# its symbols. rg2JucePlugin links rg2Lib rather than recompiling its sources, so
# a call to rg2::pch2Load or a rg2::InternalClient constructed from there fails at
# the link step unless the source is listed below.
#
# crc16.* and internalClient.* are rg2Lib sources. g2PatchLoad.* is not: it lives
# in rg2JucePlugin, and appending it here would move a plugin translation unit
# into the library.

list(APPEND RG2LIB_SOURCES
	crc16.h
	crc16.cpp)

list(APPEND RG2LIB_SOURCES
	internalClient.h
	internalClient.cpp)
