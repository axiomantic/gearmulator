# t0_timebase_header.cmake -- the driver for task SCH-0's check.
#
# Run by ctest, not by the build. It performs three cases in order and fails if
# ANY of them fails:
#
#   1. It compiles t0_timebase_header.c as C11. That is what proves the header
#      is C11 only: a C++ reference parameter, a namespace, a template or an
#      overload is a syntax error in C, and every _Static_assert in the header
#      and in the test fires here.
#   2. It runs the resulting program, which drives alloc() and framesForBlock()
#      against the sequences design sections 13.4.1 and 14.1.1 state.
#   3. It runs the grep case.
#
# The compile is a TEST-time action on purpose. A header that stopped being C11
# would otherwise break the build and report nothing through `ctest -R`, which
# plan section 7.7.1 names as a distinct class of check failure.

cmake_minimum_required(VERSION 3.15)

foreach(requiredArgument IN ITEMS
	G2_C_COMPILER G2_SOURCE G2_INCLUDE_DIR G2_WORK_DIR G2_REPO_ROOT
	G2_GIT_EXECUTABLE)
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

# ---------------- case 1: the C11 compile

if(G2_C_COMPILER_ID STREQUAL "MSVC")
	set(g2CompileCommand ${G2_C_COMPILER} /nologo /std:c11 /W4 /WX
		/I${G2_INCLUDE_DIR} ${G2_SOURCE} /Fe:${g2CompiledProgram})
else()
	set(g2CompileCommand ${G2_C_COMPILER} -std=c11 -pedantic-errors
		-Wall -Wextra -Werror
		-I${G2_INCLUDE_DIR} ${G2_SOURCE} -o ${g2CompiledProgram})
endif()

message(STATUS "t0_timebase_header case 1: ${g2CompileCommand}")

execute_process(
	COMMAND ${g2CompileCommand}
	WORKING_DIRECTORY ${G2_WORK_DIR}
	RESULT_VARIABLE g2CompileResult
	OUTPUT_VARIABLE g2CompileOutput
	ERROR_VARIABLE g2CompileError)

if(NOT g2CompileResult EQUAL 0)
	message(FATAL_ERROR
		"t0_timebase_header case 1 FAILED: g2/timebase.h did not compile as "
		"C11.\n"
		"exit code: ${g2CompileResult}\n"
		"${g2CompileOutput}${g2CompileError}")
endif()

# ---------------- case 2: run it

execute_process(
	COMMAND ${g2CompiledProgram}
	WORKING_DIRECTORY ${G2_WORK_DIR}
	RESULT_VARIABLE g2RunResult
	OUTPUT_VARIABLE g2RunOutput
	ERROR_VARIABLE g2RunError)

message(STATUS "t0_timebase_header case 2: ${g2RunOutput}${g2RunError}")

if(NOT g2RunResult EQUAL 0)
	message(FATAL_ERROR
		"t0_timebase_header case 2 FAILED: the compiled program reported a "
		"failure.\n"
		"exit code: ${g2RunResult}\n"
		"${g2RunOutput}${g2RunError}")
endif()

# ---------------- case 3: the grep case
#
# Two strings must appear NOWHERE: the debt-alarm macro that design section
# 13.4.6 deleted, and the MCU clock literal that design section 13.4.3 REFUTES
# with five independent objections. Refuted is not "unverified": an unverified
# value may turn out right and a refuted one will not, so its literal is banned
# rather than tracked.
#
# THE NEEDLES ARE ASSEMBLED FROM HALVES, and that is load-bearing rather than
# cosmetic. The scan below covers every file in this repository with NO
# exclusion list. A driver that spelled either needle out in full would match
# ITSELF, the case could never pass, and the usual repair -- excluding the
# driver from its own scan -- would open exactly the hole the case exists to
# close.
string(CONCAT g2RefutedMcuClock "540" "00000")
string(CONCAT g2DeletedDebtMacro "G2_DEBT_" "ALARM_QUANTA")

# THE SCOPE, STATED, because a scope nobody measured is the defect this kind of
# check usually carries.
#
# `git grep` here searches this repository's own tracked files plus its
# untracked, non-ignored ones. It does NOT recurse into submodules and it does
# not read ignored paths, so build/ is out of scope.
#
# That scope is chosen against a measurement, not by taste. A literal walk of
# the working tree finds the refuted MCU clock literal in FOUR vendored
# third-party files -- freetype's and zlib's crc32.h tables, and two pcre/sljit
# sources under dsp56300/wxWidgets -- where it is a coincidental substring of a
# CRC constant and has nothing to do with any clock. A check scoped to the
# literal tree could therefore never pass on a clone with submodules
# initialised. This scope is the WIDEST one that still passes, and it covers
# every file this project actually writes.
#
# -I skips binary files. --fixed-strings makes the needle a literal.

foreach(g2Needle IN ITEMS ${g2RefutedMcuClock} ${g2DeletedDebtMacro})
	execute_process(
		COMMAND ${G2_GIT_EXECUTABLE} grep -I --untracked --line-number
			--fixed-strings -e ${g2Needle}
		WORKING_DIRECTORY ${G2_REPO_ROOT}
		RESULT_VARIABLE g2GrepResult
		OUTPUT_VARIABLE g2GrepOutput
		ERROR_VARIABLE g2GrepError)

	# git grep exits 0 when it MATCHED, 1 when it did not, and >1 on an error.
	# A match is the failure here, so the exit codes are read explicitly rather
	# than through a truthiness test that would score an error as a pass.
	if(g2GrepResult EQUAL 0)
		message(FATAL_ERROR
			"t0_timebase_header case 3 FAILED: the string '${g2Needle}' is "
			"present in this repository. Design section 13.4.3 refutes the "
			"MCU clock literal and design section 13.4.6 deleted the debt "
			"alarm macro; neither may come back.\n"
			"${g2GrepOutput}")
	elseif(NOT g2GrepResult EQUAL 1)
		message(FATAL_ERROR
			"t0_timebase_header case 3 FAILED: git grep could not run, so the "
			"case is unproven rather than passed.\n"
			"exit code: ${g2GrepResult}\n"
			"${g2GrepError}")
	endif()

	message(STATUS "t0_timebase_header case 3: '${g2Needle}' is absent")
endforeach()

message(STATUS "t0_timebase_header: all three cases passed")
