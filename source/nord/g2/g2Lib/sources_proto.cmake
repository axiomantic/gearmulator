# Source list for the proto track. Owned by the proto track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.

# A test target that compiles a source directly does not prove g2Lib carries
# its symbols. g2JucePlugin links g2Lib rather than recompiling its sources, so
# a call to g2::pch2Load or a g2::InternalClient constructed from there fails at
# the link step unless the source is listed below.
#
# crc16.* and internalClient.* are g2Lib sources. g2PatchLoad.* is not: it lives
# in g2JucePlugin, and appending it here would move a plugin translation unit
# into the library.

list(APPEND G2LIB_SOURCES
	crc16.h
	crc16.cpp)

list(APPEND G2LIB_SOURCES
	internalClient.h
	internalClient.cpp)
