# Test registrations for the sched track. Owned by the sched track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ---------------- SCH-0 - t0_timebase_header
#
# The check COMPILES t0_timebase_header.c as C11 at TEST time and then runs the
# grep case. Both live in t0_timebase_header.cmake, which fails if either
# fails. The compile is not left to the build on purpose: plan section 7.7.1
# separates "the build broke" from "the check reported", and design section
# 13.4.1 makes "g2/timebase.h is a C header" a contract that has to be
# reportable through `ctest -R`.
#
# No library is linked and no C++ is involved, so this test needs no target.

find_program(G2_GIT_EXECUTABLE NAMES git)

add_test(NAME t0_timebase_header
	COMMAND ${CMAKE_COMMAND}
		-DG2_C_COMPILER=${CMAKE_C_COMPILER}
		-DG2_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}
		-DG2_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/t0_timebase_header.c
		-DG2_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
		-DG2_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/t0_timebase_header
		-DG2_REPO_ROOT=${CMAKE_SOURCE_DIR}
		-DG2_GIT_EXECUTABLE=${G2_GIT_EXECUTABLE}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/t0_timebase_header.cmake)

# ---------------- SCH-4 - t0_frame_layout
#
# An ordinary executable, because SCH-4's acceptance criterion is a BUILD-time
# property: the four fully qualified function-pointer types in
# t0_frame_layout.cpp must fail to compile when any parameter is removed from
# any one of them, and a missing conversion overload must be a link error.
# Those two failures belong to the compiler and the linker. What remains for
# ctest is the behaviour -- the sign extension, the 24-bit mask, the slot count
# and the untouched registers -- and that is what the program checks when it
# runs.

add_executable(t0_frame_layout ${CMAKE_CURRENT_SOURCE_DIR}/t0_frame_layout.cpp)
target_link_libraries(t0_frame_layout PRIVATE g2Lib)
set_property(TARGET t0_frame_layout PROPERTY FOLDER "G2/test")

add_test(NAME t0_frame_layout COMMAND t0_frame_layout)

# ---------------- SCH-1 - t0_alloc
#
# An ordinary executable. g2/timebase.h is a header with no compiled part, so
# the link against g2Lib is for the include directories the target carries and
# for nothing else.

add_executable(t0_alloc ${CMAKE_CURRENT_SOURCE_DIR}/t0_alloc.cpp)
target_link_libraries(t0_alloc PRIVATE g2Lib)
set_property(TARGET t0_alloc PROPERTY FOLDER "G2/test")

add_test(NAME t0_alloc COMMAND t0_alloc)

# ---------------- SCH-2 - t0_frames_for_block
#
# An ordinary executable, for the reason t0_alloc gives: g2/timebase.h has no
# compiled part and the link supplies the include directories.

add_executable(t0_frames_for_block
	${CMAKE_CURRENT_SOURCE_DIR}/t0_frames_for_block.cpp)
target_link_libraries(t0_frames_for_block PRIVATE g2Lib)
set_property(TARGET t0_frames_for_block PROPERTY FOLDER "G2/test")

add_test(NAME t0_frames_for_block COMMAND t0_frames_for_block)

# ---------------- SCH-3 - t0_clock_guard
#
# An ordinary executable, and every path it needs is passed on the command
# line. A path the test had to guess would be a path the test could get wrong
# in silence.
#
# The scratch header the negative case plants must live under source/nord/g2/,
# because that is the tree BRD-0's guard scans. The test writes it, runs one
# configure and removes it again.
#
# THE NEGATIVE CONFIGURE IS NOT THE WHOLE PROJECT. The test writes a scratch
# CMake project whose only content is one add_subdirectory of g2Lib, so it
# configures the real g2Lib/CMakeLists.txt -- where the guard lives -- and
# nothing else. That costs about a second.

add_executable(t0_clock_guard ${CMAKE_CURRENT_SOURCE_DIR}/t0_clock_guard.cpp)
set_property(TARGET t0_clock_guard PROPERTY FOLDER "G2/test")

add_test(NAME t0_clock_guard COMMAND t0_clock_guard
	${CMAKE_SOURCE_DIR}
	${G2_GIT_EXECUTABLE}
	${CMAKE_COMMAND}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${CMAKE_CURRENT_SOURCE_DIR}/t0_clock_guard_scratch.h
	${CMAKE_CURRENT_BINARY_DIR}/t0_clock_guard_work)

# THE WORK DIRECTORY IS NOT NAMED AFTER THE TARGET. CMAKE_CURRENT_BINARY_DIR is
# where the executable itself lands, so a work directory called t0_clock_guard
# would collide with the file t0_clock_guard. The test then cannot create it,
# every command it runs in it fails with no output, and the case reports
# failures that say nothing about the guard.

# ---------------- SCH-5 - t0_frame_conversion
#
# SCH-4's t0_frame_layout holds the four SIGNATURES, which is a build-time
# property. This one holds the BEHAVIOUR, so it is an ordinary executable that
# links the library the conversions live in.

add_executable(t0_frame_conversion
	${CMAKE_CURRENT_SOURCE_DIR}/t0_frame_conversion.cpp)
target_link_libraries(t0_frame_conversion PRIVATE g2Lib)
set_property(TARGET t0_frame_conversion PROPERTY FOLDER "G2/test")

add_test(NAME t0_frame_conversion COMMAND t0_frame_conversion)

# ---------------- SCH-6 - t0_dsp_context_layout
#
# The member list is a compile-time property, so most of this check is
# static_assert and a deleted member is a build failure. Plan section 7.7.1
# separates that from a check report, so the registered program also carries a
# runtime half: the executor's pointer recovery, and that the members are
# distinct objects.

add_executable(t0_dsp_context_layout
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_context_layout.cpp)
target_link_libraries(t0_dsp_context_layout PRIVATE g2Lib)
set_property(TARGET t0_dsp_context_layout PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_context_layout COMMAND t0_dsp_context_layout)

# ---------------- SCH-10 - t0_esai_frame
#
# An ordinary executable. The fixture builds a real dsp56k::Memory, two
# PeripheralsNop and a real dsp56k::DSP, because both ESAI control-register
# writes reach through the peripheral set to the DSP.

add_executable(t0_esai_frame ${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_frame.cpp)
target_link_libraries(t0_esai_frame PRIVATE g2Lib)
set_property(TARGET t0_esai_frame PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_frame COMMAND t0_esai_frame)

# ---------------- SCH-15 - t0_codec_queue_surface
#
# An ordinary executable. A BUILD of the target tests neither the surface nor
# the refusal, so both belong to a registered program that runs. The address of
# every declared method is taken through its fully qualified type, so a renamed
# or re-signed method is a compile error and a declared-and-undefined one is a
# link error.

add_executable(t0_codec_queue_surface
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_queue_surface.cpp)
target_link_libraries(t0_codec_queue_surface PRIVATE g2Lib)
set_property(TARGET t0_codec_queue_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_queue_surface COMMAND t0_codec_queue_surface)

# ---------------- SCH-7 - t0_executor
#
# An ordinary executable. The declared names are held by their fully
# qualified types inside the translation unit, so the compiler and the linker
# carry that half. What remains for ctest is the behaviour: the order of the
# jobs, that every job ran on the CALLING thread, and that a refused
# re-entry is counted. The re-entry count is an observable in every build type,
# because the default build here is Release with NDEBUG and a check whose
# predicate is "the debug build asserted" cannot fail in it.

add_executable(t0_executor ${CMAKE_CURRENT_SOURCE_DIR}/t0_executor.cpp)
target_link_libraries(t0_executor PRIVATE g2Lib)
set_property(TARGET t0_executor PROPERTY FOLDER "G2/test")

add_test(NAME t0_executor COMMAND t0_executor)

# ---------------- SCH-8 - t0_run_dsp_cycles_contract
#
# An ordinary executable. The declared signature is held by the whole function
# type inside the translation unit, so the compiler carries that half and a
# declared-and-undefined function is a link error. What remains for ctest is
# the SHAPE OF THE LOOP: a wantCycles of 0 executes no exec() at all, and a
# wantCycles of 1 against a block that costs more than 1 executes exactly one.
# Both are read from the DSP's own instruction and cycle counters, so neither
# depends on an assertion that a release build removes.

add_executable(t0_run_dsp_cycles_contract
	${CMAKE_CURRENT_SOURCE_DIR}/t0_run_dsp_cycles_contract.cpp)
target_link_libraries(t0_run_dsp_cycles_contract PRIVATE g2Lib)
set_property(TARGET t0_run_dsp_cycles_contract PROPERTY FOLDER "G2/test")

add_test(NAME t0_run_dsp_cycles_contract COMMAND t0_run_dsp_cycles_contract)

# ---------------- SCH-9 - t0_run_dsp_cycles
#
# An ordinary executable. SCH-8's t0_run_dsp_cycles_contract holds the declared
# signature and the shape of the loop; this one drives the BOUND over 1,000
# quanta for each of four scripted block lengths that straddle the budget. The
# upper half of the bound comes from the fixture's own largest measured
# dispatch unit, never from maxInstructionsPerBlock, which the shipped
# configuration leaves uncapped.

add_executable(t0_run_dsp_cycles
	${CMAKE_CURRENT_SOURCE_DIR}/t0_run_dsp_cycles.cpp)
target_link_libraries(t0_run_dsp_cycles PRIVATE g2Lib)
set_property(TARGET t0_run_dsp_cycles PROPERTY FOLDER "G2/test")

add_test(NAME t0_run_dsp_cycles COMMAND t0_run_dsp_cycles)

# ---------------- SCH-14 - t0_block_table_harness
#
# An ordinary executable, and the path of the committed synthetic program is
# passed on the command line. A path the check had to guess would be a path the
# check could get wrong in silence.
#
# THIS ROW ESTABLISHES NO maxDispatchCost. It verifies the instrument against a
# program whose longest block is known by construction. SCH-31 is the
# measurement that reads the real compiled kernel, and it is T1.

add_executable(t0_block_table_harness
	${CMAKE_CURRENT_SOURCE_DIR}/t0_block_table_harness.cpp)
target_link_libraries(t0_block_table_harness PRIVATE g2Lib)
set_property(TARGET t0_block_table_harness PROPERTY FOLDER "G2/test")

add_test(NAME t0_block_table_harness COMMAND t0_block_table_harness
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/synthetic_block_program.asm)

# ---------------- SCH-16 - t0_codec_capacity
#
# An ordinary executable. SCH-15's t0_codec_queue_surface holds the declared
# members and the refusal; this one drives the CAPACITY ARITHMETIC over 1,000
# blocks and carries the negative cases that prove the counters can fire.
# The sink is primed with the lookahead, without which a capacity of B alone
# would pass every assertion and the L + B rule would go untested.

add_executable(t0_codec_capacity
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_capacity.cpp)
target_link_libraries(t0_codec_capacity PRIVATE g2Lib)
set_property(TARGET t0_codec_capacity PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_capacity COMMAND t0_codec_capacity)

# ---------------- SCH-17 - t0_backend_rule
#
# An ordinary executable, because SCH-17's acceptance criterion is the
# NULL-vs-non-null distinction of Scheduler::create. The test reads
# dsp56k::g_useJIT at run time, so the Backend::Jit case is conditional on
# the build and the Backend::Interpreter case is unconditional. The test
# prints the build mode in its first line so the outcome is traceable
# without a separate device.
#
# No library beyond g2Lib is linked, and the g2Lib target carries the
# dsp56kEmu include directories transitively, so the test reaches
# dsp56k::g_useJIT through scheduler.h.

add_executable(t0_backend_rule ${CMAKE_CURRENT_SOURCE_DIR}/t0_backend_rule.cpp)
target_link_libraries(t0_backend_rule PRIVATE g2Lib)
set_property(TARGET t0_backend_rule PROPERTY FOLDER "G2/test")

add_test(NAME t0_backend_rule COMMAND t0_backend_rule)

# ---------------- SCH-12 - t0_cycle_debt
#
# An ordinary executable. SCH-12 declares g2::runQuantum as a function
# template in g2Lib/cycleDebt.h, and this check drives the rule itself -- the
# invariant 0 <= debt < maxDispatchCost, the floor at zero, the never-idle
# zero-drift case and the forced-idle slow-and-bounded case, plus the want <= 0
# branch -- against a SYNTHETIC context and SYNTHETIC role-filler. The bound
# comes from the test's own fixture (maxDispatchCost is measurement register
# row 1 and has no committed value), the context exposes exactly the four
# members the template needs, and the role-filler models spent as emulated
# cycles only, never a wall clock. The "one block used twice" acceptance
# criterion is discharged at the two call sites SCH-11 and SCH-30, which this
# task does not write.

add_executable(t0_cycle_debt ${CMAKE_CURRENT_SOURCE_DIR}/t0_cycle_debt.cpp)
target_link_libraries(t0_cycle_debt PRIVATE g2Lib)
set_property(TARGET t0_cycle_debt PROPERTY FOLDER "G2/test")

add_test(NAME t0_cycle_debt COMMAND t0_cycle_debt)

# ---------------- SCH-13 - t0_long_dispatch
#
# An ordinary executable. SCH-13 drives rule 4 of design section 13.4.6: a
# SYNTHETIC context whose single dispatch unit costs more than one frame's
# allocation drives the want <= 0 branch of the g2::runQuantum template
# (SCH-12). Each branch quantum raises longDispatchQuanta by EXACTLY one, pays
# the debt down by one whole allocation, and never invokes the role-filler;
# once the spike is paid down the running branch resumes and the debt returns
# inside the fixture's own rule 2 bound. No case reads a clock, and the bound
# comes from this fixture, never from maxInstructionsPerBlock (which the
# shipped configuration leaves uncapped).

add_executable(t0_long_dispatch
	${CMAKE_CURRENT_SOURCE_DIR}/t0_long_dispatch.cpp)
target_link_libraries(t0_long_dispatch PRIVATE g2Lib)
set_property(TARGET t0_long_dispatch PROPERTY FOLDER "G2/test")

add_test(NAME t0_long_dispatch COMMAND t0_long_dispatch)

# ---------------- SCH-11 - t0_dsp_job_order
#
# An ordinary executable. The whole check is the ORDER inside dspJob, and it
# is asserted through the fixture's ordered callback log on the audio and
# second Esai. The signature half is held by a static_assert against the
# Executor's JobFn inside the translation unit. The debt-consumed quantum
# never reaches a DSP, so the context carries a NULL dsp on purpose and no
# JIT backend is required -- which is why this check can run in any build.

add_executable(t0_dsp_job_order
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_job_order.cpp)
target_link_libraries(t0_dsp_job_order PRIVATE g2Lib)
set_property(TARGET t0_dsp_job_order PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_job_order COMMAND t0_dsp_job_order)

# ---------------- SCH-33 - t0_dsp_run_gate
#
# An ordinary executable. Unlike t0_dsp_job_order above, the context here
# carries a live DSP running a scripted loop, so this check needs a JIT backend.

add_executable(t0_dsp_run_gate
	${CMAKE_CURRENT_SOURCE_DIR}/t0_dsp_run_gate.cpp)
target_link_libraries(t0_dsp_run_gate PRIVATE g2Lib)
set_property(TARGET t0_dsp_run_gate PROPERTY FOLDER "G2/test")

add_test(NAME t0_dsp_run_gate COMMAND t0_dsp_run_gate)

# ---------------- SCH-32 - t0_status_contract
#
# An ordinary executable. The compile-time half of the contract -- the scoping,
# the non-conversion to int and the fixed underlying type -- is held by
# static_asserts, so a violation there is a BUILD failure and this target is
# what carries it. The run-time half -- the zero value, the roster against
# Status::Count and the distinguishable failures -- reports through the test's
# own failure counter, because the default build type is Release and Release
# defines NDEBUG.
#
# It does NOT link g2Lib. status.h is a header with no compiled part and the
# test reaches it through the include directory alone, so linking the library
# would make this check wait on every other source in it without buying the
# check anything.

add_executable(t0_status_contract
	${CMAKE_CURRENT_SOURCE_DIR}/t0_status_contract.cpp)
target_include_directories(t0_status_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_property(TARGET t0_status_contract PROPERTY FOLDER "G2/test")

add_test(NAME t0_status_contract COMMAND t0_status_contract)

# ---------------- SCH-18 - t0_construction_rejection
#
# An ordinary executable. Every rejectable value arrives through
# Scheduler::Config, so the check drives one Config for each row of the plan's
# rejection table and asserts BOTH halves of the row: a null return AND the
# exact g2::Status. It links g2Lib because the factory now lives in
# scheduler.cpp, and because the check constructs a real Board and the serial
# Executor to satisfy the widened signature.
#
# NDEBUG CHANGES NO CASE IN IT, and that is a property of how the check is
# written rather than one this registration exercises. Nothing in it is an
# assert() and nothing in it catches an exception, so the status out-param is the
# whole observable in either build type.
#
# WHAT THIS REGISTRATION DOES NOT COVER: the generator here is single-config and
# the tree is configured Debug, so this add_test names one build type and can
# name no other. A second tree configured -DCMAKE_BUILD_TYPE=Release, or a
# multi-config generator, running this same registration is what would cover the
# release half. This file configures no such tree.
#
# The Backend::Interpreter row is unconditional; the success cases are
# conditional on dsp56k::g_useJIT, for the reason t0_backend_rule already
# records above.

add_executable(t0_construction_rejection
	${CMAKE_CURRENT_SOURCE_DIR}/t0_construction_rejection.cpp)
target_link_libraries(t0_construction_rejection PRIVATE g2Lib)
set_property(TARGET t0_construction_rejection PROPERTY FOLDER "G2/test")

add_test(NAME t0_construction_rejection COMMAND t0_construction_rejection)

# ---------------- SCH-19 - t0_order
#
# An ordinary executable. It links g2Lib because the whole check drives a real
# Board, a real DSP set and the real ChainAdapter through Scheduler::runFrames;
# the check supplies only the two objects the factory already takes by
# injection -- an Executor and, through Config::trace, a TraceSink.
#
# NDEBUG CHANGES NO CASE IN IT. Nothing in it is an assert() and nothing in it
# catches an exception, so the failure counter is the whole observable in either
# build type. WHAT THIS REGISTRATION DOES NOT COVER: the generator here is
# single-config and the tree is configured Debug, so this add_test names one
# build type and can name no other.

add_executable(t0_order ${CMAKE_CURRENT_SOURCE_DIR}/t0_order.cpp)
target_link_libraries(t0_order PRIVATE g2Lib)
set_property(TARGET t0_order PROPERTY FOLDER "G2/test")

add_test(NAME t0_order COMMAND t0_order)

# ---------------- SCH-34 - t0_esai_slot_phase
#
# An ordinary executable. The path of the committed fixture is passed on the
# command line, the same way SCH-14's t0_block_table_harness passes
# synthetic_block_program.asm. The test reads the path from argv[1] and
# assembles the file at run time through dsp56k::Assembler.
#
# THE TEST MUST RUN UNDER THE JIT. g_useJIT is read at run time and the
# test fails loudly rather than skip on a non-JIT build. The fixture spin
# sits below Vba_End ($100) so dynamicFastInterrupts (set in DspSet::Slot::Slot
# by DSP-19's production code) puts the JIT in FastInterruptMode::Dynamic and
# exec() returns after each instruction. The test uses DspSet rather than
# PeripheralsNop because dynamicFastInterrupts is set in DspSet::Slot::Slot.

add_executable(t0_esai_slot_phase
	${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_slot_phase.cpp)
target_link_libraries(t0_esai_slot_phase PRIVATE g2Lib)
set_property(TARGET t0_esai_slot_phase PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_slot_phase COMMAND t0_esai_slot_phase
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures/esai_sync_spin.asm)
set_tests_properties(t0_esai_slot_phase PROPERTIES LABELS "UnitTest")

# ---------------- SCH-35 - t0_esai_idle_core
#
# An ordinary executable over a fixture-free source: the discriminating case's
# guest is the reset state itself and the companion case enables ports through
# the public control-register writes, so no .asm is committed. The program is
# assembled in-test and sits below Vba_End for the same reason
# t0_esai_slot_phase's does.

add_executable(t0_esai_idle_core
	${CMAKE_CURRENT_SOURCE_DIR}/t0_esai_idle_core.cpp)
target_link_libraries(t0_esai_idle_core PRIVATE g2Lib)
set_property(TARGET t0_esai_idle_core PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_idle_core COMMAND t0_esai_idle_core)
set_tests_properties(t0_esai_idle_core PROPERTIES LABELS "UnitTest")

# ---------------- SCH-29 - t0_transport_hub
#
# An ordinary executable. A BUILD of the target sees none of the six properties
# this check owns: the allocation total, that nothing allocates after
# construction, the two refusals, the fixed attachment order, the stamp and the
# borrow lifetime. All six report through the test's own failure counter, so
# NDEBUG changes no case in it -- nothing in the source is an assert() and
# nothing catches an exception. The compile-time half is the member-function
# pointers and the static_asserts, which the compiler and the linker carry.
#
# THE HEADER PATH IS PASSED ON THE COMMAND LINE, the way t0_clock_guard and
# t0_block_table_harness pass theirs. The declaration-order property -- that
# ProtocolFrame and StampedFrame are declared BEFORE TransportHub -- is a
# property of the header's source text that no C++ expression can read, so the
# check reads the file. A path the test had to guess would be a path the test
# could get wrong in silence.

add_executable(t0_transport_hub
	${CMAKE_CURRENT_SOURCE_DIR}/t0_transport_hub.cpp)
target_link_libraries(t0_transport_hub PRIVATE g2Lib)
set_property(TARGET t0_transport_hub PROPERTY FOLDER "G2/test")

add_test(NAME t0_transport_hub COMMAND t0_transport_hub
	${CMAKE_CURRENT_SOURCE_DIR}/../transportHub.h)
set_tests_properties(t0_transport_hub PROPERTIES LABELS "UnitTest")

# ---------------- SCH-21 step 1 - t0_begin_play_phase
#
# An ordinary executable. It drives a real Board, a real DSP set, the real
# ChainAdapter and both real codec queues through Scheduler::runFrames and
# Scheduler::beginPlayPhase; the check supplies only the two objects the
# factory already takes by injection -- an Executor and, through Config::trace,
# a TraceSink.
#
# NDEBUG CHANGES NO CASE IN IT. Nothing in the source is an assert() and
# nothing catches an exception, so the failure counter is the whole observable
# in either build type. WHAT THIS REGISTRATION DOES NOT COVER: the generator
# here is single-config and the tree is configured Debug, so this add_test
# names one build type and can name no other.

add_executable(t0_begin_play_phase
	${CMAKE_CURRENT_SOURCE_DIR}/t0_begin_play_phase.cpp)
target_link_libraries(t0_begin_play_phase PRIVATE g2Lib)
set_property(TARGET t0_begin_play_phase PROPERTY FOLDER "G2/test")

add_test(NAME t0_begin_play_phase COMMAND t0_begin_play_phase)
set_tests_properties(t0_begin_play_phase PROPERTIES LABELS "UnitTest")

# ---------------- SCH-21 step 2 - t0_codec_regimes
#
# An ordinary executable. It drives a real Board, a real DSP set, the real
# ChainAdapter and both real codec queues through Scheduler::runFrames and
# Scheduler::beginPlayPhase, and observes the two play-only phases through the
# same Config::trace sink SCH-19 declares. A BUILD of the target sees none of
# the four properties this check owns: the boot regime's five records, the play
# regime's seven, the POSITION of the ingress and the egress within design
# section 13.5's order, and the negative case in which a play regime run during
# what would be the boot fills the sink and stops the scheduler.
#
# NDEBUG CHANGES NO CASE IN IT. Nothing in the source is an assert() and
# nothing catches an exception, so the failure counter is the whole observable
# in either build type.

add_executable(t0_codec_regimes
	${CMAKE_CURRENT_SOURCE_DIR}/t0_codec_regimes.cpp)
target_link_libraries(t0_codec_regimes PRIVATE g2Lib)
set_property(TARGET t0_codec_regimes PROPERTY FOLDER "G2/test")

add_test(NAME t0_codec_regimes COMMAND t0_codec_regimes)
set_tests_properties(t0_codec_regimes PROPERTIES LABELS "UnitTest")

# ---------------- SCH-21 step 3 - t0_scheduler_faults
#
# An ordinary executable. It drives a real Board, its real DSP set, the real
# ChainAdapter and both real codec queues; the ONE object it supplies is the
# Executor, which the factory already takes by injection. That Executor
# dispatches every real job and then writes one JobFault into one context,
# which is where design section 13.10.5 puts a fault and where the Scheduler
# reads it -- so the fault path under test is the production one.
#
# A BUILD OF THE TARGET SEES NONE OF THE FIVE PROPERTIES THIS CHECK OWNS: the
# latch, its stickiness, the context index it names, the removal of the faulted
# context from the dispatch set, and reset() restoring that set.
#
# NDEBUG CHANGES NO CASE IN IT. Nothing in the source is an assert() and
# nothing catches an exception, so the failure counter is the whole observable
# in either build type.

add_executable(t0_scheduler_faults
	${CMAKE_CURRENT_SOURCE_DIR}/t0_scheduler_faults.cpp)
target_link_libraries(t0_scheduler_faults PRIVATE g2Lib)
set_property(TARGET t0_scheduler_faults PROPERTY FOLDER "G2/test")

add_test(NAME t0_scheduler_faults COMMAND t0_scheduler_faults)
set_tests_properties(t0_scheduler_faults PROPERTIES LABELS "UnitTest")

# ---------------- SCH-21 step 5 - t0_thread_map
#
# An ordinary executable. It drives a real Board, its real DSP set, the real
# ChainAdapter and both real codec queues through the two phases of the thread
# map, and it runs the AUDIO phase on a real second thread -- which is the half
# no single-threaded fixture can distinguish from "records the first thread it
# ever saw".
#
# THE ASSERTION IN scheduler.cpp IS NOT THIS CHECK'S PREDICATE. Every case here
# reads the recorded identity back through Scheduler::owningThread, which is
# present in every build type, so the property that stops the data race in the
# SHIPPED build is the property this check reads. No case in the source is an
# assert() and no case catches an exception.
#
# WHAT THIS REGISTRATION DOES NOT COVER: the generator here is single-config
# and the tree is configured Debug, so this add_test names one build type and
# can name no other. The source carries no build-type-dependent case, which is
# what makes the single registration sufficient rather than merely convenient.
#
# NOTHING IN THE SOURCE READS docs/threading.md. That document is the map; a
# test that grepped it would assert the prose and pass while the code was wrong.

add_executable(t0_thread_map
	${CMAKE_CURRENT_SOURCE_DIR}/t0_thread_map.cpp)
target_link_libraries(t0_thread_map PRIVATE g2Lib)
set_property(TARGET t0_thread_map PROPERTY FOLDER "G2/test")

add_test(NAME t0_thread_map COMMAND t0_thread_map)
set_tests_properties(t0_thread_map PROPERTIES LABELS "UnitTest")

# ---------------- SCH-21 step 6 - t0_mcu_debt
#
# An ordinary executable. It drives a real Board whose SDRAM window holds a
# field of one repeated instruction, its real MCF5307 core and the real
# Scheduler; the ONE object it supplies is the Executor, which the factory
# already takes by injection.
#
# IT LINKS g2Lib AND THEREFORE THE REAL mcf5307 CORE, AND ITS FIRST CASE IS
# ABOUT THAT LIBRARY RATHER THAN ABOUT THE SCHEDULER. A core that clamped
# mcf5307_exec's return to its budget makes design section 13.4.6's cycle debt
# identically zero -- section 24.6 row W3-410 -- so case 1 offers the linked
# core a budget of one cycle and asserts it reports more. Every later case in
# the file is vacuous without that property, and the file stops rather than
# reporting them if the measurement is unusable.
#
# NO CYCLE COST IS COMPILED IN. The cost and the byte length of one dispatch
# unit are measured at run time from the linked core, and both workload rates,
# every expected spend and design section 13.4.6's rule 2 bound are derived
# from them.
#
# A BUILD OF THE TARGET SEES ONLY THE COMPILE-TIME HALF: the four static_asserts
# that hold McuContext's members to the types the shared block reads. The seven
# run-time properties -- the overshoot, the debt walk, the floor at zero, rule
# 4's counter, the rule 2 bound, the conservation law and the agreement with
# g2::runQuantum itself -- all report through the failure counter.
#
# NDEBUG CHANGES NO CASE IN IT. Nothing in the source is an assert() and nothing
# catches an exception, so the failure counter is the whole observable in either
# build type.

add_executable(t0_mcu_debt
	${CMAKE_CURRENT_SOURCE_DIR}/t0_mcu_debt.cpp)
target_link_libraries(t0_mcu_debt PRIVATE g2Lib)
set_property(TARGET t0_mcu_debt PROPERTY FOLDER "G2/test")

add_test(NAME t0_mcu_debt COMMAND t0_mcu_debt)
set_tests_properties(t0_mcu_debt PROPERTIES LABELS "UnitTest")
