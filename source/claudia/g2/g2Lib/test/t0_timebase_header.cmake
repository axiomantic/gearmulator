# t0_timebase_header.cmake -- the driver for the g2/timebase.h check.
#
# Run by ctest, not by the build. It compiles t0_timebase_header.c as C11 and
# then runs it, and fails if either step fails.
#
# The compile is a test-time action on purpose. A header that stopped being C11
# would otherwise break the build and report nothing through `ctest -R`, which
# is a distinct class of check failure. Compiling as C is what proves the header
# is C11 only: a C++ reference parameter, a namespace, a template or an overload
# is a syntax error in C, and every _Static_assert in the header and in the test
# fires at that step.

cmake_minimum_required(VERSION 3.15)

foreach(requiredArgument IN ITEMS
	G2_C_COMPILER G2_SOURCE G2_INCLUDE_DIR G2_WORK_DIR)
	if(NOT DEFINED ${requiredArgument}
		OR "${${requiredArgument}}" STREQUAL ""
		OR "${${requiredArgument}}" MATCHES "NOTFOUND$")
		message(FATAL_ERROR
			"t0_timebase_header: ${requiredArgument} was not supplied "
			"(value: '${${requiredArgument}}'). The check cannot run, and it "
			"reports that rather than passing vacuously.")
	endif()
endforeach()

file(MAKE_DIRECTORY ${G2_WORK_DIR})
set(g2CompiledProgram ${G2_WORK_DIR}/t0_timebase_header_c11)

# ---------------- the C11 compile

if(G2_C_COMPILER_ID STREQUAL "MSVC")
	set(g2CompileCommand ${G2_C_COMPILER} /nologo /std:c11 /W4 /WX
		/I${G2_INCLUDE_DIR} ${G2_SOURCE} /Fe:${g2CompiledProgram})
else()
	set(g2CompileCommand ${G2_C_COMPILER} -std=c11 -pedantic-errors
		-Wall -Wextra -Werror
		-I${G2_INCLUDE_DIR} ${G2_SOURCE} -o ${g2CompiledProgram})
endif()

message(STATUS "t0_timebase_header compile: ${g2CompileCommand}")

execute_process(
	COMMAND ${g2CompileCommand}
	WORKING_DIRECTORY ${G2_WORK_DIR}
	RESULT_VARIABLE g2CompileResult
	OUTPUT_VARIABLE g2CompileOutput
	ERROR_VARIABLE g2CompileError)

if(NOT g2CompileResult EQUAL 0)
	message(FATAL_ERROR
		"t0_timebase_header FAILED: g2/timebase.h did not compile as C11.\n"
		"exit code: ${g2CompileResult}\n"
		"${g2CompileOutput}${g2CompileError}")
endif()

# ---------------- run it

execute_process(
	COMMAND ${g2CompiledProgram}
	WORKING_DIRECTORY ${G2_WORK_DIR}
	RESULT_VARIABLE g2RunResult
	OUTPUT_VARIABLE g2RunOutput
	ERROR_VARIABLE g2RunError)

message(STATUS "t0_timebase_header run: ${g2RunOutput}${g2RunError}")

if(NOT g2RunResult EQUAL 0)
	message(FATAL_ERROR
		"t0_timebase_header FAILED: the compiled program reported a failure.\n"
		"exit code: ${g2RunResult}\n"
		"${g2RunOutput}${g2RunError}")
endif()
