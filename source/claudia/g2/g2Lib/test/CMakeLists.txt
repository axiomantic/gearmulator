cmake_minimum_required(VERSION 3.15)

# A track adds its own add_test(NAME <name> ...) lines to its own per-track list
# below and to nothing else.

# Copy elision is PERMITTED, not required, so a test whose callbacks outlive the
# frame they captured passes only while the compiler chooses to elide. Disabling
# it turns that lifetime into something this directory checks rather than
# something a compiler grants. C++17's guaranteed prvalue elision is unaffected,
# so correct code is not slowed or broken by it. The flag is C++-only and its
# spelling differs per compiler, hence the language-and-id condition rather than
# a bare option. It precedes every include below because add_compile_options
# reaches only targets declared after it.
add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-fno-elide-constructors>")

# MSVC spells it /Zc:nrvo-, introduced in VS 2022 17.4. cl KEEPS COMPILING
# through an unrecognised option, reporting only warning D9002, so an
# unconditional flag would be ignored in silence on an older toolchain and this
# directory would advertise a guard it does not have. Both conditions below must
# therefore hold: the version test does not depend on the CMake release carrying
# a D9002 pattern in check_cxx_compiler_flag's FAIL_REGEX list, and the probe
# does not depend on the version table.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
	set(_g2_nrvo_gap "")

	if(MSVC_VERSION LESS 1934)
		set(_g2_nrvo_gap "MSVC_VERSION ${MSVC_VERSION} is below 1934, the VS 2022 17.4 that introduced /Zc:nrvo-")
	else()
		include(CheckCXXCompilerFlag)
		check_cxx_compiler_flag("/Zc:nrvo-" G2_TEST_HAVE_ZC_NRVO)

		if(NOT G2_TEST_HAVE_ZC_NRVO)
			set(_g2_nrvo_gap "this compiler did not accept /Zc:nrvo-")
		endif()
	endif()

	if(_g2_nrvo_gap)
		message(WARNING
			"g2Lib/test: copy elision stays ENABLED because ${_g2_nrvo_gap}. A test whose "
			"callbacks outlive the frame they captured passes here on elision rather than "
			"being checked.")
	else()
		add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:nrvo->")
	endif()

	unset(_g2_nrvo_gap)
endif()

include(tests_board.cmake)
include(tests_sched.cmake)
include(tests_chain.cmake)
include(tests_dsp.cmake)
include(tests_repo.cmake)
include(tests_plugin.cmake)
include(tests_proto.cmake)
include(tests_oracle.cmake)
include(tests_perf.cmake)
include(tests_int.cmake)

# The aggregate build target. `cmake --build <build> --target
# g2_test_executables_all` builds this directory's executables and their link
# libraries and stops there; a bare build of the tree also builds
# source/3rdparty, which none of these tests need.
#
# One aggregate target rather than the whole executable list passed to --target:
# under the Makefile generator each --target argument is its own recursive make
# and its own walk of the graph, so the list form costs an order of magnitude
# more even with nothing out of date.

get_property(_g2_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)

set(_g2_test_executables "")

foreach(_g2_target IN LISTS _g2_test_targets)
	get_target_property(_g2_target_type ${_g2_target} TYPE)

	if(_g2_target_type STREQUAL "EXECUTABLE")
		list(APPEND _g2_test_executables ${_g2_target})
	endif()
endforeach()

if(_g2_test_executables)
	add_custom_target(g2_test_executables_all)
	add_dependencies(g2_test_executables_all ${_g2_test_executables})
endif()

unset(_g2_target)	# CMP0124 is unset (so it behaves as OLD) and foreach() leaks the loop variable into directory scope
unset(_g2_target_type)
unset(_g2_test_targets)
unset(_g2_test_executables)
