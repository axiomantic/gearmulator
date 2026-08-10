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
# cosmetic. This driver is a tracked file inside the tree the scan covers, and
# nothing below excludes it. A driver that spelled either needle out in full
# would match ITSELF, the case could never pass, and the usual repair --
# excluding the driver from its own scan -- would open exactly the hole the case
# exists to close.
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
#
# ---------------- WHAT IS NOT SOURCE, AND HOW IT IS FOUND
#
# THE SCAN MUST NOT READ BUILD OUTPUT, AND A DIRECTORY NAME IS NOT A RELIABLE
# WAY TO TELL. This was measured rather than argued.
#
# `--untracked` covers untracked, non-ignored files. `.gitignore` ignores
# /build/ and nothing else, so a build tree at that ONE path is out of scope and
# a build tree at ANY OTHER PATH is not. A CTest log quotes the output of the
# tests it ran, and the failure message below PRINTS the refuted value in full.
# So a second build tree, under any name but `build`, puts the literal into
# <tree>/Testing/Temporary/LastTest.log and this case reports it as present:
# THE CHECK'S OWN OUTPUT BECOMES ITS INPUT. Measured on this repository: with
# the untracked build trees `build-baseline/` and `build-pins/` on disk, the
# case reported five matches and every one of them was a log line -- one of them
# a log of a run that had quoted the OTHER tree's log.
#
# A check that passes because of what somebody called a directory is an
# accident, not a check. The scope is therefore stated by a PROPERTY of the
# directory and never by its name: A DIRECTORY THAT HOLDS A CMakeCache.txt IS A
# CMake BUILD TREE, and a build tree is output rather than source. The set is
# computed at run time, so a build tree created after this file was written is
# excluded too, and a directory that merely LOOKS like build output is not.
#
# THE EXCLUDED SET IS PRINTED WITH THE RESULT -- on the passing path and in
# every failure message -- so the scope is never invisible to whoever reads the
# verdict. An exclusion nobody can see is indistinguishable from a check that
# stopped working.
#
# WHAT THIS DOES NOT COVER, stated rather than implied: a build tree that is not
# a wholly untracked directory -- an IN-SOURCE configure at the repository root
# is the real instance. That tree has no untracked directory to exclude, and
# excluding the root would exclude the whole scan. Such a tree makes this case
# report a FALSE POSITIVE, which is loud, rather than a false pass, which is
# silent. That is the correct direction for the failure to point.

execute_process(
	COMMAND ${G2_GIT_EXECUTABLE} ls-files --others --directory
		--exclude-standard
	WORKING_DIRECTORY ${G2_REPO_ROOT}
	RESULT_VARIABLE g2ListingResult
	OUTPUT_VARIABLE g2ListingOutput
	ERROR_VARIABLE g2ListingError)

set(g2BuildTreeExclusions "")
set(g2ExcludedNames "")

if(g2ListingResult EQUAL 0)
	string(REPLACE "\n" ";" g2ListedEntries "${g2ListingOutput}")

	foreach(g2Entry IN LISTS g2ListedEntries)
		# git reports a directory with a trailing slash and a file without one.
		if(NOT g2Entry MATCHES "/$")
			continue()
		endif()

		if(NOT EXISTS "${G2_REPO_ROOT}/${g2Entry}CMakeCache.txt")
			continue()
		endif()

		list(APPEND g2BuildTreeExclusions ":(exclude)${g2Entry}")
		list(APPEND g2ExcludedNames "${g2Entry}")
	endforeach()
endif()

if(g2ExcludedNames STREQUAL "")
	string(CONCAT g2ScopeReport
		"t0_timebase_header case 3 scope: excluded build trees: NONE. No "
		"untracked directory holds a CMakeCache.txt.")
else()
	string(REPLACE ";" " " g2ExcludedNamesText "${g2ExcludedNames}")
	string(CONCAT g2ScopeReport
		"t0_timebase_header case 3 scope: excluded build trees, each because it "
		"holds a CMakeCache.txt: ${g2ExcludedNamesText}")
endif()

# The listing is what makes the exclusion computable. If it could not run the
# scan still proceeds -- a WIDER scan can only produce a false positive, never a
# false pass -- but the report says so rather than implying an exclusion set
# that was never computed.
if(NOT g2ListingResult EQUAL 0)
	string(CONCAT g2ScopeReport
		"${g2ScopeReport} WARNING: git ls-files exited ${g2ListingResult}, so "
		"no exclusion could be computed and the scan below is WIDER than the "
		"rule specifies: ${g2ListingError}")
endif()

message(STATUS "${g2ScopeReport}")

set(g2GrepPathspec "")
if(NOT g2BuildTreeExclusions STREQUAL "")
	set(g2GrepPathspec -- . ${g2BuildTreeExclusions})
endif()

foreach(g2Needle IN ITEMS ${g2RefutedMcuClock} ${g2DeletedDebtMacro})
	execute_process(
		COMMAND ${G2_GIT_EXECUTABLE} grep -I --untracked --line-number
			--fixed-strings -e ${g2Needle} ${g2GrepPathspec}
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
			"${g2ScopeReport}\n"
			"${g2GrepOutput}")
	elseif(NOT g2GrepResult EQUAL 1)
		message(FATAL_ERROR
			"t0_timebase_header case 3 FAILED: git grep could not run, so the "
			"case is unproven rather than passed.\n"
			"${g2ScopeReport}\n"
			"exit code: ${g2GrepResult}\n"
			"${g2GrepError}")
	endif()

	message(STATUS "t0_timebase_header case 3: '${g2Needle}' is absent")
endforeach()

message(STATUS "t0_timebase_header: all three cases passed")
