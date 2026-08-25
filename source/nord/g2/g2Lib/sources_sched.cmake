# Source list for the sched track. Owned by the sched track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.
#
# Created empty by task BRD-0.

# SCH-4. Frame.h is the single conversion point between g2::Frame and the two
# dsp56k::Audio frame types; nothing else in g2Lib names a library frame type.
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

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/scheduler.h
	${CMAKE_CURRENT_SOURCE_DIR}/scheduler.cpp)

# cycleDebt.h declares the g2::runQuantum function template, the one shared
# budget/want/debt block applied once for a DSP context and once for the MCU.
# It has no compiled part; it is listed so the file appears in the target.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/cycleDebt.h)

# dspJob.cpp is the DSP job body: receive, the cycle-debt block (which
# instantiates the g2::runQuantum template rather than re-implementing it),
# transmit.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspJob.cpp)

# SCH-29. transportHub.* is the transport hub: the fixed-allocation, fixed-order
# path between the device and the three attachments of design section 15.1.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/transportHub.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/transportHub.h)
