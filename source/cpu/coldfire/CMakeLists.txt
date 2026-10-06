cmake_minimum_required(VERSION 3.15)

project(coldfire)

if(NOT TARGET mcf5407::mcf5407)
	if(RG2_MCF5407_SOURCE_DIR)
		option(RG2_MCF5407_SOURCE_DIR_IS_NOT_THE_PIN "Build from RG2_MCF5407_SOURCE_DIR even when that checkout is not the pinned commit" OFF)
		find_program(G2_GIT_EXECUTABLE NAMES git)

		set(mcf5407HeadIsPin FALSE)
		set(mcf5407Divergence "")
		execute_process(
			COMMAND ${G2_GIT_EXECUTABLE} -C "${RG2_MCF5407_SOURCE_DIR}" rev-parse HEAD
			OUTPUT_VARIABLE mcf5407Head OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET RESULT_VARIABLE mcf5407HeadResult)
		execute_process(
			COMMAND ${G2_GIT_EXECUTABLE} -C "${RG2_MCF5407_SOURCE_DIR}" rev-parse "${RG2_MCF5407_GIT_TAG}^{commit}"
			OUTPUT_VARIABLE mcf5407Pin OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET RESULT_VARIABLE mcf5407PinResult)
		execute_process(
			COMMAND ${G2_GIT_EXECUTABLE} -C "${RG2_MCF5407_SOURCE_DIR}" status --porcelain
			OUTPUT_VARIABLE mcf5407Dirty OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET RESULT_VARIABLE mcf5407DirtyResult)

		if(NOT mcf5407HeadResult EQUAL 0 OR NOT mcf5407DirtyResult EQUAL 0)
			set(mcf5407Divergence "its state could not be read: `git -C <dir> rev-parse HEAD` or `git status` failed, so this tree cannot be compared against the pin at all")
		elseif(NOT mcf5407PinResult EQUAL 0)
			set(mcf5407Divergence "it does not contain the pinned commit ${RG2_MCF5407_GIT_TAG} at all; its HEAD is ${mcf5407Head}")
		elseif(NOT mcf5407Head STREQUAL mcf5407Pin)
			set(mcf5407Divergence "its HEAD is ${mcf5407Head}, and the pin is ${mcf5407Pin}")
		elseif(NOT mcf5407Dirty STREQUAL "")
			set(mcf5407Divergence "its working tree has uncommitted changes, so what would compile is not the pinned commit even though its HEAD is")
		else()
			set(mcf5407HeadIsPin TRUE)
		endif()

		if(NOT mcf5407HeadIsPin AND NOT RG2_MCF5407_SOURCE_DIR_IS_NOT_THE_PIN)
			message(FATAL_ERROR
				"RG2_MCF5407_SOURCE_DIR points at ${RG2_MCF5407_SOURCE_DIR}, but "
				"${mcf5407Divergence}. Every result from this build tree would be a "
				"statement about that tree and not about the pin.\n"
				"Unset RG2_MCF5407_SOURCE_DIR to build against the pin, check the "
				"sibling out onto ${RG2_MCF5407_GIT_TAG}, or pass "
				"-DRG2_MCF5407_SOURCE_DIR_IS_NOT_THE_PIN=ON to build against it "
				"deliberately — and then label every result from this build tree as "
				"not testing the pin.")
		endif()
		if(NOT mcf5407HeadIsPin)
			message(WARNING
				"mcf5407: building against ${RG2_MCF5407_SOURCE_DIR}, not the pin, because "
				"${mcf5407Divergence}. Results from this build tree do not describe the pin.")
		endif()

		add_subdirectory(${RG2_MCF5407_SOURCE_DIR} ${CMAKE_BINARY_DIR}/mcf5407 EXCLUDE_FROM_ALL)
	else()
		include(FetchContent)
		FetchContent_MakeAvailable(mcf5407)
	endif()
endif()

set(COLDFIRE_SOURCES
	busTarget.h
	interruptController.h
	interruptController.cpp
	mcf5407Soc.h
	mcf5407Soc.cpp
	sim.h
	sim.cpp
	timer.h
	timer.cpp
	uart0.h
	uart0.cpp
)

add_library(coldfire STATIC ${COLDFIRE_SOURCES})
source_group("source" FILES ${COLDFIRE_SOURCES})
set_property(TARGET coldfire PROPERTY FOLDER "cpu")

target_compile_features(coldfire PUBLIC cxx_std_20)
target_include_directories(coldfire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(coldfire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(coldfire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(TARGET mcf5407::mcf5407)
	target_include_directories(coldfire PUBLIC
		$<TARGET_PROPERTY:mcf5407::mcf5407,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
