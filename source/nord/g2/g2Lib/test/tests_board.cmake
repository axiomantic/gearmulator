# Test registrations for the board track. Owned by the board track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- BRD-1: MemoryMap
add_executable(t0_memory_map t0_memory_map.cpp)
target_link_libraries(t0_memory_map PRIVATE g2Lib)
set_property(TARGET t0_memory_map PROPERTY FOLDER "G2")
add_test(NAME t0_memory_map COMMAND t0_memory_map)
set_tests_properties(t0_memory_map PROPERTIES LABELS "UnitTest")

# ----------------- BRD-3: InterruptController
add_executable(t0_interrupt_controller t0_interrupt_controller.cpp)
target_link_libraries(t0_interrupt_controller PRIVATE g2Lib)
set_property(TARGET t0_interrupt_controller PROPERTY FOLDER "G2")
add_test(NAME t0_interrupt_controller COMMAND t0_interrupt_controller)
set_tests_properties(t0_interrupt_controller PROPERTIES LABELS "UnitTest")

# ----------------- BRD-7: Flash
add_executable(t0_flash t0_flash.cpp)
target_link_libraries(t0_flash PRIVATE g2Lib)
set_property(TARGET t0_flash PROPERTY FOLDER "G2")
add_test(NAME t0_flash COMMAND t0_flash)
set_tests_properties(t0_flash PROPERTIES LABELS "UnitTest")

# ----------------- BRD-16: Hdi08Adapter
add_executable(t0_hdi08_adapter t0_hdi08_adapter.cpp)
target_link_libraries(t0_hdi08_adapter PRIVATE g2Lib)
set_property(TARGET t0_hdi08_adapter PROPERTY FOLDER "G2")
add_test(NAME t0_hdi08_adapter COMMAND t0_hdi08_adapter)
set_tests_properties(t0_hdi08_adapter PROPERTIES LABELS "UnitTest")

# ----------------- BRD-21: Board::runMcu no-op
add_executable(t0_board_runmcu t0_board_runmcu.cpp)
target_link_libraries(t0_board_runmcu PRIVATE g2Lib)
set_property(TARGET t0_board_runmcu PROPERTY FOLDER "G2")
add_test(NAME t0_board_runmcu COMMAND t0_board_runmcu)
set_tests_properties(t0_board_runmcu PROPERTIES LABELS "UnitTest")

