# Test registrations for the chain track. Owned by the chain track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- SlotWriteView

add_executable(t0_slot_write_view t0_slot_write_view.cpp)
target_link_libraries(t0_slot_write_view PRIVATE g2Lib)
set_property(TARGET t0_slot_write_view PROPERTY FOLDER "G2/test")

add_test(NAME t0_slot_write_view COMMAND t0_slot_write_view)
set_tests_properties(t0_slot_write_view PROPERTIES LABELS "UnitTest")


# ----------------- the mailbox surface

add_executable(t0_mailbox_surface t0_mailbox_surface.cpp)
target_link_libraries(t0_mailbox_surface PRIVATE g2Lib)
set_property(TARGET t0_mailbox_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_surface COMMAND t0_mailbox_surface)
set_tests_properties(t0_mailbox_surface PROPERTIES LABELS "UnitTest")


# ----------------- the mailbox index test

add_executable(t0_mailbox_index t0_mailbox_index.cpp)
target_link_libraries(t0_mailbox_index PRIVATE g2Lib)
set_property(TARGET t0_mailbox_index PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_index COMMAND t0_mailbox_index)
set_tests_properties(t0_mailbox_index PROPERTIES LABELS "UnitTest")

# ----------------- ChainTopology and mailboxCount

add_executable(t0_mailbox_count t0_mailbox_count.cpp)
target_link_libraries(t0_mailbox_count PRIVATE g2Lib)
set_property(TARGET t0_mailbox_count PROPERTY FOLDER "G2/test")

add_test(NAME t0_mailbox_count COMMAND t0_mailbox_count)
set_tests_properties(t0_mailbox_count PROPERTIES LABELS "UnitTest")


# ----------------- the ChainAdapter surface

add_executable(t0_chain_adapter_surface t0_chain_adapter_surface.cpp)
target_link_libraries(t0_chain_adapter_surface PRIVATE g2Lib)
set_property(TARGET t0_chain_adapter_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_adapter_surface COMMAND t0_chain_adapter_surface)
set_tests_properties(t0_chain_adapter_surface PROPERTIES LABELS "UnitTest")


# ----------------- the written-flag rule

add_executable(t0_written_flag t0_written_flag.cpp)
target_link_libraries(t0_written_flag PRIVATE g2Lib)
set_property(TARGET t0_written_flag PROPERTY FOLDER "G2/test")

add_test(NAME t0_written_flag COMMAND t0_written_flag)
set_tests_properties(t0_written_flag PROPERTIES LABELS "UnitTest")


# ----------------- advanceAll and the four ordered steps

add_executable(t0_advance_all t0_advance_all.cpp)
target_link_libraries(t0_advance_all PRIVATE g2Lib)
set_property(TARGET t0_advance_all PROPERTY FOLDER "G2/test")

add_test(NAME t0_advance_all COMMAND t0_advance_all)
set_tests_properties(t0_advance_all PROPERTIES LABELS "UnitTest")


# ----------------- the three counters

add_executable(t0_chain_counters t0_chain_counters.cpp)
target_link_libraries(t0_chain_counters PRIVATE g2Lib)
set_property(TARGET t0_chain_counters PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_counters COMMAND t0_chain_counters)
set_tests_properties(t0_chain_counters PROPERTIES LABELS "UnitTest")


# ----------------- t0_four_phase, the four-phase procedure and the codec edges
#
# Part A drives a real Scheduler over a real Board and reads the phase order of
# one quantum out of the trace: the swap is the first record, the run phase
# follows, and the dispatch runs DSP 0 to DSP 7 in g2::kJobCount ascending
# positions. Part B drives the two codec edges against the adapter that owns
# them: the ingress writes slots 0 and 1 of mailbox 0's read frame through
# ingressFrame() and carries no delay of its own, the egress reads slots 0 and 1
# of the tail mailbox's write frame through egressFrame(), the
# swap-before-ingress and run-before-egress orders are each shown to be
# load-bearing, and a Ring runs neither edge.

add_executable(t0_four_phase t0_four_phase.cpp)
target_link_libraries(t0_four_phase PRIVATE g2Lib)
set_property(TARGET t0_four_phase PROPERTY FOLDER "G2/test")

add_test(NAME t0_four_phase COMMAND t0_four_phase)
set_tests_properties(t0_four_phase PROPERTIES LABELS "UnitTest")


# ----------------- t0_chain_state, the state round trip
#
# Runs 100 quanta, saves, runs 100 more, loads, runs the same 100 again and
# asserts identical mailbox contents and identical counters. The empty-snapshot
# defect is refused explicitly: the digest at the save point must differ from
# the digest a hundred quanta later, the counters must have risen above zero,
# and stateSize() must grow strictly with the ring depth.

add_executable(t0_chain_state t0_chain_state.cpp)
target_link_libraries(t0_chain_state PRIVATE g2Lib)
set_property(TARGET t0_chain_state PROPERTY FOLDER "G2/test")

add_test(NAME t0_chain_state COMMAND t0_chain_state)
set_tests_properties(t0_chain_state PROPERTIES LABELS "UnitTest")
