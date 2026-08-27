# Test registrations for the chain track. Owned by the chain track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- CHN-3, SlotWriteView
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_slot_write_view$

add_executable(t0_slot_write_view t0_slot_write_view.cpp)
target_link_libraries(t0_slot_write_view PRIVATE g2Lib)
set_property(TARGET t0_slot_write_view PROPERTY FOLDER "G2/test")

add_test(NAME t0_slot_write_view COMMAND t0_slot_write_view)
set_tests_properties(t0_slot_write_view PROPERTIES LABELS "UnitTest")


# ----------------- CHN-1, the mailbox surface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mailbox_surface$

add_executable(t0_mailbox_surface t0_mailbox_surface.cpp)
target_link_libraries(t0_mailbox_surface PRIVATE g2Lib)
set_property(TARGET t0_mailbox_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_surface COMMAND t0_mailbox_surface)
set_tests_properties(t0_mailbox_surface PROPERTIES LABELS "UnitTest")


# ----------------- CHN-2, the mailbox index test
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mailbox_index$

add_executable(t0_mailbox_index t0_mailbox_index.cpp)
target_link_libraries(t0_mailbox_index PRIVATE g2Lib)
set_property(TARGET t0_mailbox_index PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_index COMMAND t0_mailbox_index)
set_tests_properties(t0_mailbox_index PROPERTIES LABELS "UnitTest")

# ----------------- CHN-4, ChainTopology and mailboxCount
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_mailbox_count$
#
# The registered test asserts mailboxCount in a CONSTANT EXPRESSION: the
# mailbox arrays are sized from exactly such a use, so a declaration-only
# function compiles and links a target and fails only at the constant-
# expression use (plan section 7.7.1). A static_assert is the only check
# that can catch it.

add_executable(t0_mailbox_count t0_mailbox_count.cpp)
target_link_libraries(t0_mailbox_count PRIVATE g2Lib)
set_property(TARGET t0_mailbox_count PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_count COMMAND t0_mailbox_count)
set_tests_properties(t0_mailbox_count PROPERTIES LABELS "UnitTest")


# ----------------- CHN-5, the ChainAdapter surface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_chain_adapter_surface$
#
# Constructs one adapter with each of the four constructor arguments set to a
# distinct value and asserts each is forwarded and readable; asserts the audio
# chain reports exactly dspCount + 1 mailboxes at every second-bus topology;
# and takes the address of every method on the declared public surface, so a
# missing one is a link error.

add_executable(t0_chain_adapter_surface t0_chain_adapter_surface.cpp)
target_link_libraries(t0_chain_adapter_surface PRIVATE g2Lib)
set_property(TARGET t0_chain_adapter_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_adapter_surface COMMAND t0_chain_adapter_surface)
set_tests_properties(t0_chain_adapter_surface PROPERTIES LABELS "UnitTest")


# ----------------- CHN-6, the written-flag rule
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_written_flag$
#
# The transmit wrappers' written flag records WHICH KIND of delivery arrived --
# good, stale, or none -- and is driven by the emulated ESAI's own
# frame-lifetime transmit-underrun latch, Esai::txUnderrunInFrame(), read at the
# instant the callback fires; it is NOT the callback's arrival (section 12.3).
# The test constructs real dsp56k::Esai objects, PLANTS A REAL TRANSMIT UNDERRUN
# through the peripheral's transmit path, fires each position's transmit wrapper
# and reads the flag back through ChainAdapter::audioWritten / secondWritten,
# asserting the per-position and per-bus separation directly. It used to poke the
# status register instead, which proved the read discriminates and never that the
# condition can occur.

add_executable(t0_written_flag t0_written_flag.cpp)
target_link_libraries(t0_written_flag PRIVATE g2Lib)
set_property(TARGET t0_written_flag PROPERTY FOLDER "G2/test")

add_test(NAME t0_written_flag COMMAND t0_written_flag)
set_tests_properties(t0_written_flag PROPERTIES LABELS "UnitTest")


# ----------------- CHN-7, advanceAll and the four ordered steps
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_advance_all$
#
# advanceAll closes the underrun accounting for the quantum that just ended:
# (1) every quantum, count audio-bus underruns from clear audio flags; (2)
# only when frameIndex % secondBusFrameDivider == 0, count second-bus
# underruns; (3) clear the audio flags always and the second-bus flags only
# on the window quanta; (4) advance() the selected mailboxes. The test drives
# the real CHN-6 flags through a divider of 2 and asserts the counters, the
# per-bus clear cadence, and the second-bus mailbox-advance gate.

add_executable(t0_advance_all t0_advance_all.cpp)
target_link_libraries(t0_advance_all PRIVATE g2Lib)
set_property(TARGET t0_advance_all PROPERTY FOLDER "G2/test")

add_test(NAME t0_advance_all COMMAND t0_advance_all)
set_tests_properties(t0_advance_all PROPERTIES LABELS "UnitTest")


# ----------------- CHN-8, the counters
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_chain_counters$
#
# Asserts the properties this row owns: (1) underrunFrames and
# secondBusUnderrunFrames are separate storage, driven one above zero at a
# single position while the other stays zero there; (2) one unwanted callback
# raises phaseErrorFrames(position) by exactly ONE even when both conditions
# (already delivered this quantum AND non-window) hold at once.

add_executable(t0_chain_counters t0_chain_counters.cpp)
target_link_libraries(t0_chain_counters PRIVATE g2Lib)
set_property(TARGET t0_chain_counters PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_counters COMMAND t0_chain_counters)
set_tests_properties(t0_chain_counters PROPERTIES LABELS "UnitTest")


# ----------------- CHN-9 STEP 1, the four-phase procedure and the codec edges
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_four_phase$
#
# PART A drives a real Scheduler over a real Board and reads the phase order of
# one quantum out of the SCH-19 trace: the swap is the FIRST record, the run
# phase follows, and step 3 dispatches DSP 0 to DSP 7 in g2::kJobCount
# ascending positions. PART B drives the two codec edges against the adapter
# that owns them: the ingress writes slots 0 and 1 of mailbox 0's READ frame
# through ingressFrame() and carries no delay of its own, the egress reads
# slots 0 and 1 of the tail mailbox's WRITE frame through egressFrame(), the
# swap-before-ingress and run-before-egress orders are each shown to be
# load-bearing, and a Ring runs neither edge.

add_executable(t0_four_phase t0_four_phase.cpp)
target_link_libraries(t0_four_phase PRIVATE g2Lib)
set_property(TARGET t0_four_phase PROPERTY FOLDER "G2/test")

add_test(NAME t0_four_phase COMMAND t0_four_phase)
set_tests_properties(t0_four_phase PROPERTIES LABELS "UnitTest")


# ----------------- CHN-9 STEP 2 (the absorbed CHN-14), the state round trip
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_chain_state$
#
# Runs 100 quanta, saves, runs 100 more, loads, runs the same 100 again and
# asserts identical mailbox contents and identical counters. The empty-snapshot
# defect is refused explicitly: the digest at the save point must DIFFER from
# the digest a hundred quanta later, all three counters must have risen above
# zero, and stateSize() must grow strictly with the ring depth.

add_executable(t0_chain_state t0_chain_state.cpp)
target_link_libraries(t0_chain_state PRIVATE g2Lib)
set_property(TARGET t0_chain_state PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_state COMMAND t0_chain_state)
set_tests_properties(t0_chain_state PROPERTIES LABELS "UnitTest")


# ----------------- CHN-6 / CHN-7, the underrun gate's KNOWN POSITIVE
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_esai_underrun_gate$
#
# t0_written_flag proves the transmit wrappers can READ a stale-frame signal;
# it drives that signal by writing the ESAI status register by hand and so
# never proves the CONDITION can occur. This row plants a REAL transmit
# underrun through the emulated peripheral -- a TX register left unwritten for
# one slot, then refilled before the frame boundary exactly as the DMA does --
# and asserts underrunFrames and secondBusUnderrunFrames rise from it, while
# clean quanta leave them alone. It also carries phaseErrorFrames' known
# positive and the case that keeps the underrun rule from blinding the
# phase-error rule.

add_executable(t0_esai_underrun_gate t0_esai_underrun_gate.cpp)
target_link_libraries(t0_esai_underrun_gate PRIVATE g2Lib)
set_property(TARGET t0_esai_underrun_gate PROPERTY FOLDER "G2/test")

add_test(NAME t0_esai_underrun_gate COMMAND t0_esai_underrun_gate)
set_tests_properties(t0_esai_underrun_gate PROPERTIES LABELS "UnitTest")
