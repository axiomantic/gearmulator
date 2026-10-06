cmake_minimum_required(VERSION 3.15)

project(coldfireSoc)

if(NOT TARGET coldfire::coldfire)
	if(RG2_COLDFIRE_SOURCE_DIR)
		option(RG2_COLDFIRE_SOURCE_DIR_IS_NOT_THE_PIN "Build from RG2_COLDFIRE_SOURCE_DIR even when that checkout is not the pinned commit" OFF)
		find_program(G2_GIT_EXECUTABLE NAMES git)

		set(coldfireHeadIsPin FALSE)
		set(coldfireDivergence "")
		execute_process(
			COMMAND ${G2_GIT_EXECUTABLE} -C "${RG2_COLDFIRE_SOURCE_DIR}" rev-parse HEAD
			OUTPUT_VARIABLE coldfireHead OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET RESULT_VARIABLE coldfireHeadResult)
		execute_process(
			COMMAND ${G2_GIT_EXECUTABLE} -C "${RG2_COLDFIRE_SOURCE_DIR}" rev-parse "${RG2_COLDFIRE_GIT_TAG}^{commit}"
			OUTPUT_VARIABLE coldfirePin OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET RESULT_VARIABLE coldfirePinResult)
		execute_process(
			COMMAND ${G2_GIT_EXECUTABLE} -C "${RG2_COLDFIRE_SOURCE_DIR}" status --porcelain
			OUTPUT_VARIABLE coldfireDirty OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET RESULT_VARIABLE coldfireDirtyResult)

		if(NOT coldfireHeadResult EQUAL 0 OR NOT coldfireDirtyResult EQUAL 0)
			set(coldfireDivergence "its state could not be read: `git -C <dir> rev-parse HEAD` or `git status` failed, so this tree cannot be compared against the pin at all")
		elseif(NOT coldfirePinResult EQUAL 0)
			set(coldfireDivergence "it does not contain the pinned commit ${RG2_COLDFIRE_GIT_TAG} at all; its HEAD is ${coldfireHead}")
		elseif(NOT coldfireHead STREQUAL coldfirePin)
			set(coldfireDivergence "its HEAD is ${coldfireHead}, and the pin is ${coldfirePin}")
		elseif(NOT coldfireDirty STREQUAL "")
			set(coldfireDivergence "its working tree has uncommitted changes, so what would compile is not the pinned commit even though its HEAD is")
		else()
			set(coldfireHeadIsPin TRUE)
		endif()

		if(NOT coldfireHeadIsPin AND NOT RG2_COLDFIRE_SOURCE_DIR_IS_NOT_THE_PIN)
			message(FATAL_ERROR
				"RG2_COLDFIRE_SOURCE_DIR points at ${RG2_COLDFIRE_SOURCE_DIR}, but "
				"${coldfireDivergence}. Every result from this build tree would be a "
				"statement about that tree and not about the pin.\n"
				"Unset RG2_COLDFIRE_SOURCE_DIR to build against the pin, check the "
				"sibling out onto ${RG2_COLDFIRE_GIT_TAG}, or pass "
				"-DRG2_COLDFIRE_SOURCE_DIR_IS_NOT_THE_PIN=ON to build against it "
				"deliberately — and then label every result from this build tree as "
				"not testing the pin.")
		endif()
		if(NOT coldfireHeadIsPin)
			message(WARNING
				"coldfire: building against ${RG2_COLDFIRE_SOURCE_DIR}, not the pin, because "
				"${coldfireDivergence}. Results from this build tree do not describe the pin.")
		endif()

		add_subdirectory(${RG2_COLDFIRE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/coldfire EXCLUDE_FROM_ALL)
	else()
		include(FetchContent)
		FetchContent_MakeAvailable(coldfire)
	endif()
endif()

set(COLDFIRE_SOURCES
	busTarget.h
	interruptController.h
	interruptController.cpp
	coldfireSoc.h
	coldfireSoc.cpp
	sim.h
	sim.cpp
	timer.h
	timer.cpp
	uart0.h
	uart0.cpp
)

add_library(coldfireSoc STATIC ${COLDFIRE_SOURCES})
source_group("source" FILES ${COLDFIRE_SOURCES})
set_property(TARGET coldfireSoc PROPERTY FOLDER "cpu")

target_compile_features(coldfireSoc PUBLIC cxx_std_20)
target_include_directories(coldfireSoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(coldfireSoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(coldfireSoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(TARGET coldfire::coldfire)
	target_include_directories(coldfireSoc PUBLIC
		$<TARGET_PROPERTY:coldfire::coldfire,INTERFACE_INCLUDE_DIRECTORIES>
	)
endif()
