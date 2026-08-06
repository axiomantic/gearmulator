# Source list for the board track. Owned by the board track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.

list(APPEND G2LIB_SOURCES
	memoryMap.cpp
	interruptController.cpp
	flash.cpp
	hdi08Adapter.cpp
	board.cpp
)
