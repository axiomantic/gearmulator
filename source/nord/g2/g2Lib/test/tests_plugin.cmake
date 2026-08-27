# Test registrations for the plugin track. Owned by the plugin track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- g2TestConsole's subcommand surface
#
# Tier T0. Every child it spawns runs with NMG2_ARTIFACTS unset, so it boots no
# firmware and needs no artifact.
#
# It reads two things and holds no roster: the subcommand block `--help` prints,
# and the `command == "--name"` comparisons in main.cpp. The rule is that the
# two sets are equal, so the source path is a compile definition rather than a
# copied list.
#
# g2TestConsole lives in a sibling directory that is added after this one, so
# the path arrives as a generator expression and the build order as an explicit
# dependency. Without the dependency the fixture that builds this directory's
# executables would leave the console binary from the previous generation in
# place, and the test would read a stale surface.

# Registered only when g2TestConsole is part of this configure, and the
# condition is a question about the build and not about the source tree.
# t0_clock_guard configures a scratch project whose whole content is one
# add_subdirectory of g2Lib, so this file is read there too -- and there
# g2TestConsole is never added and $<TARGET_FILE:g2TestConsole> fails the
# generate step, taking that unrelated test red. `EXISTS` on the sibling path
# cannot discriminate the two: it is the same source tree in both. What
# discriminates them is g2Lib's parent directory: source/nord/g2 in the real
# build, which adds g2TestConsole beside g2Lib, and t0_clock_guard's scratch
# directory otherwise.
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

	# ----------------- `--impulse` reports an outcome word
	#
	# Tier T0. Its child runs with NMG2_ARTIFACTS unset, so it boots no firmware.
	#
	# It holds that a machine which never ran, a chain that carried nothing, and
	# an observer that saw nothing must not print the same thing.
	add_executable(t0_impulse_outcome t0_impulse_outcome.cpp)
	set_property(TARGET t0_impulse_outcome PROPERTY FOLDER "G2/test")
	target_compile_definitions(t0_impulse_outcome PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:g2TestConsole>")
	target_include_directories(t0_impulse_outcome PRIVATE "${g2_parentOfG2Lib}")
	add_dependencies(t0_impulse_outcome g2TestConsole)

	add_test(NAME t0_impulse_outcome COMMAND t0_impulse_outcome)
	set_tests_properties(t0_impulse_outcome PROPERTIES LABELS "UnitTest")

	# ----------------- `--impulse` waits for the audio path, not for the loader
	#
	# Check: ctest --test-dir build --no-tests=error -R ^t1_rx_armed$
	#
	# TIER T1 AND GATED. The child boots the real firmware, so the test resolves
	# NMG2_ARTIFACTS through ArtifactResolver and reports the section 18.5 skip
	# line when it is absent. It links g2Lib for the resolver and the gated
	# fixture and for nothing else.
	#
	# NO PLAN BLOCK OWNS THIS REGISTRATION YET, exactly as t0_impulse_outcome
	# above records for itself. The behaviour it holds -- that the boot drive
	# leaves on an observation of the ESAI receive DMA and not on programLanded
	# -- has no other mechanism, and the gap between the two predicates is a
	# factor of five in this firmware. The owning block is OWED.
	#
	# THE GATE VARIABLES ARE COMPUTED HERE rather than borrowed from
	# tests_int.cmake, which CMakeLists.txt includes AFTER this file, so the
	# variables do not exist yet at this point. The skip code is READ OUT OF
	# gatedFixture.h by the same regex tests_int.cmake and tests_board.cmake
	# use, so the three spellings cannot drift; NMG2_ARTIFACTS is a cache
	# variable, so whichever include site sets it first wins.
	#
	# TIMEOUT 600 because the drive is now roughly five times longer: the
	# receive path does not arm until boot iteration 231,296.

	add_executable(t1_rx_armed t1_rx_armed.cpp)
	target_link_libraries(t1_rx_armed PRIVATE g2Lib)
	set_property(TARGET t1_rx_armed PROPERTY FOLDER "G2/test")
	target_compile_definitions(t1_rx_armed PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:g2TestConsole>")
	add_dependencies(t1_rx_armed g2TestConsole)

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

	file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_rxArmedSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

	if(NOT g2_rxArmedSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
		message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
	endif()

	set(g2_rxArmedSkipExitCode "${CMAKE_MATCH_1}")

	if(DEFINED ENV{NMG2_ARTIFACTS})
		set(g2_rxArmedArtifactsDefault "$ENV{NMG2_ARTIFACTS}")
	else()
		get_filename_component(g2_rxArmedArtifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
	endif()

	set(NMG2_ARTIFACTS "${g2_rxArmedArtifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

	add_test(NAME t1_rx_armed COMMAND t1_rx_armed)
	set_tests_properties(t1_rx_armed PROPERTIES LABELS "IntegrationTest" TIMEOUT 600 SKIP_RETURN_CODE ${g2_rxArmedSkipExitCode})

	if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
		set_property(TEST t1_rx_armed APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	endif()
else()
	message(STATUS "g2TestConsole is not part of this configure; t0_console_subcommands and t0_impulse_outcome are not registered")
endif()
