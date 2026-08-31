# Source list for the board track. Owned by the board track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.
#
# Created empty by task BRD-0.

list(APPEND G2LIB_SOURCES memoryMap.cpp)
list(APPEND G2LIB_SOURCES sim.cpp)
list(APPEND G2LIB_SOURCES panel.cpp)
list(APPEND G2LIB_SOURCES latches.cpp)
list(APPEND G2LIB_SOURCES hdi08Decode.cpp)

# Header-only. Listed here for the IDE source group, like flash.h below.
list(APPEND G2LIB_SOURCES anomalyLog.h)

list(APPEND G2LIB_SOURCES firmwareExtract.cpp)

list(APPEND G2LIB_SOURCES
	flash.h
	flash.cpp
)
