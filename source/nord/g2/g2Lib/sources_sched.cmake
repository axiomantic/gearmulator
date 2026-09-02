# Source list for the sched track. Owned by the sched track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

# frame.h is the conversion point between g2::Frame and the dsp56k::Audio
# frame types.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/frame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/frame.h)

# Header-only, listed so the file appears in the target.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspContext.h)

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.h)

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.h)

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/executor.h
	${CMAKE_CURRENT_SOURCE_DIR}/serialExecutor.cpp)

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/runDspCycles.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/runDspCycles.h)

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/tools/blockTableHarness.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/tools/blockTableHarness.h)

# Header-only, listed so the file appears in the target.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/cycleDebt.h)

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspJob.cpp)

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/transportHub.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/transportHub.h)
