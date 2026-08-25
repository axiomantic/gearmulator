# Test registrations for the plugin track. Owned by the plugin track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- PLG-14, g2TestConsole's subcommand surface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_console_subcommands$
#
# TIER T0. Every child it spawns runs with NMG2_ARTIFACTS UNSET, so it boots no
# firmware and needs no artifact.
#
# IT READS TWO THINGS AND HOLDS NO ROSTER: the subcommand block `--help` prints,
# and the `command == "--name"` comparisons in main.cpp. PLG-14's rule is that
# the two sets are EQUAL, so the source path is a compile definition rather than
# a copied list -- a list here would be the roster W3-406 withdrew, written a
# second time.
#
# g2TestConsole lives in a SIBLING directory that is added AFTER this one, so
# the path arrives as a generator expression and the build order as an explicit
# dependency. Without the dependency the fixture that builds this directory's
# executables would leave the console binary from the previous generation in
# place, and the test would read a stale surface.

# ONLY WHEN g2TestConsole IS PART OF THIS CONFIGURE, AND THE CONDITION IS A
# QUESTION ABOUT THE BUILD AND NOT ABOUT THE SOURCE TREE. t0_clock_guard
# configures a scratch project whose whole content is one add_subdirectory of
# g2Lib, so this file is read there too -- and there g2TestConsole is never
# added and $<TARGET_FILE:g2TestConsole> fails the GENERATE step, taking that
# unrelated test red. `EXISTS` on the sibling path cannot discriminate the two:
# it is the same source tree in both. What discriminates them is g2Lib's PARENT
# DIRECTORY: source/nord/g2 in the real build, which adds g2TestConsole beside
# g2Lib, and t0_clock_guard's scratch directory otherwise.
get_directory_property(g2_parentOfG2Lib DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." PARENT_DIRECTORY)

if(EXISTS "${g2_parentOfG2Lib}/g2TestConsole/CMakeLists.txt")
	add_executable(t0_console_subcommands t0_console_subcommands.cpp)
	set_property(TARGET t0_console_subcommands PROPERTY FOLDER "G2/test")
	target_compile_definitions(t0_console_subcommands PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:g2TestConsole>"
		G2_TEST_CONSOLE_SOURCE="${g2_parentOfG2Lib}/g2TestConsole/main.cpp")
	add_dependencies(t0_console_subcommands g2TestConsole)

	add_test(NAME t0_console_subcommands COMMAND t0_console_subcommands)
	set_tests_properties(t0_console_subcommands PROPERTIES LABELS "UnitTest")
else()
	message(STATUS "g2TestConsole is not part of this configure; t0_console_subcommands is not registered")
endif()
