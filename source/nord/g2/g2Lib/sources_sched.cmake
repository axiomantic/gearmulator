# Source list for the sched track. Owned by the sched track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.
#
# Created empty by task BRD-0.

# SCH-4. frame.h is the single conversion point between g2::Frame and the two
# dsp56k::Audio frame types; nothing else in g2Lib names a library frame type.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/frame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/frame.h
	${CMAKE_CURRENT_SOURCE_DIR}/executor.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/dspContext.cpp)
