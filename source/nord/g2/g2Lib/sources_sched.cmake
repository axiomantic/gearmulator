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

# SCH-6. dspContext.h declares JobFault, JobContext and DspContext. It has no
# compiled part; it is listed so that the file the sched track owns appears in
# the target, exactly as SCH-4's frame.h does.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspContext.h)

# SCH-10. esaiFrame.* advances one whole ESAI frame in each direction. These
# two calls are what replaces an EsaiClock: the scheduler drives the frame.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.h)

# SCH-15. codecQueues.* are the two bounded queues between the Device and the
# chain. Both carry the capacity lookaheadFrames + B.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.h)
