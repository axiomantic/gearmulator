# Source list for the board track. Owned by the board track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.
#
# Created empty by task BRD-0.

# ----------------- BRD-7, the flash model
#
# flash.h declares the read-only flash surface and flash.cpp carries the
# implementation. The model is constructed with two (base, size) pairs the
# fixture supplies: no shipped header carries a chip-select base literal.

list(APPEND G2LIB_SOURCES
	flash.h
	flash.cpp
)
