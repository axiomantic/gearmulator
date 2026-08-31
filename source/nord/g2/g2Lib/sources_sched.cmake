# Source list for the sched track. Owned by the sched track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.

list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/frame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/frame.h)

# SCH-6. dspContext.h declares JobFault, JobContext and DspContext. It has no
# compiled part; it is listed so that the file the sched track owns appears in
# the target, exactly as SCH-4's frame.h does.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspContext.h)

# SCH-10. esaiFrame.* advances one whole ESAI frame in each direction. These
# calls are what replaces an EsaiClock: the scheduler drives the frame.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/esaiFrame.h)

# codecQueues.* are the bounded queues between the Device and the
# chain. Both carry the capacity lookaheadFrames + B.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/codecQueues.h)

# SCH-7. executor.h declares the Executor interface and the serial executor;
# serialExecutor.cpp defines the serial one. It runs the jobs in order ON THE
# calling thread and creates no thread.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/executor.h
	${CMAKE_CURRENT_SOURCE_DIR}/serialExecutor.cpp)

# SCH-8. runDspCycles.* is the DSP-side run call: the ctx.run(want) of design
# section 13.4.6 for a DSP context. dsp56k::DSP has no budgeted call, so this
# is an adapter this project writes.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/runDspCycles.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/runDspCycles.h)

# SCH-14. tools/blockTableHarness.* walks every entry of a compiled block table
# and reports the largest encoded cycle count. ITS TIER FOLLOWS ITS INPUT: T0
# against SCH-14's committed synthetic program, T1 against the real compiled
# kernel, which is SCH-31's measurement. It establishes no maxDispatchCost.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/tools/blockTableHarness.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/tools/blockTableHarness.h)

# SCH-17. scheduler.h declares the Backend enum and the minimum Scheduler
# factory for the single backend rule. The header is listed here so the
# build system knows it; the
# implementation is inline in the header so scheduler.cpp does not yet
# exist.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/scheduler.h)

# SCH-12. cycleDebt.h declares the g2::runQuantum function template -- design
# section 13.4.6's budget/want/debt block. It has no compiled
# part; it is listed so that the file the sched track owns appears in the
# target, exactly as SCH-4's frame.h and SCH-6's dspContext.h do.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/cycleDebt.h)

# SCH-11. dspJob.cpp is the DSP job body: receive, the cycle-debt block
# (which INSTANTIATES SCH-12's g2::runQuantum template, not re-implements
# it), transmit. Design sections 13.10.3 and 13.4.6.
list(APPEND G2LIB_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/dspJob.cpp)
