# Source list for the sched track. Owned by the sched track.
#
# Append every source name this track adds under source/claudia/rg2/rg2Lib/ to
# RG2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

# frame.h is the conversion point between rg2::Frame and the dsp56k::Audio
# frame types.
list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/frame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/frame.h)

# Header-only, listed so the file appears in the target.
list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspContext.h)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.h)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.h)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/executor.h
	${CMAKE_CURRENT_SOURCE_DIR}/serialExecutor.cpp)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/runDspCycles.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/runDspCycles.h)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/tools/blockTableHarness.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/tools/blockTableHarness.h)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/scheduler.h
	${CMAKE_CURRENT_SOURCE_DIR}/scheduler.cpp)

# Header-only, listed so the file appears in the target.
list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/cycleDebt.h)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspJob.cpp)

list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/transportHub.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/transportHub.h)

# Header-only, listed so the file appears in the target.
list(APPEND RG2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/mcuContext.h)
