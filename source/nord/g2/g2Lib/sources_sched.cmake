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
	${CMAKE_CURRENT_SOURCE_DIR}/frame.h)

# SCH-17. scheduler.h declares the Backend enum and the minimum Scheduler
# factory for the single backend rule. SCH-19 opens scheduler.cpp and grows
# the class. The header is listed here so the build system knows it; the
# implementation is inline in the header so scheduler.cpp does not yet
# exist.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/scheduler.h)
