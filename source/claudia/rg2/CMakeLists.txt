cmake_minimum_required(VERSION 3.15)

set(RG2_MCF5407_SOURCE_DIR "" CACHE PATH "Build the MCF5407 core from this checkout instead of fetching it")
set(RG2_MCF5407_GIT_TAG "babb7dea5d8e8d29dac91f29067c1384640de557" CACHE STRING "The commit or tag of axiomantic/mcf5407 to fetch")

include(FetchContent)
FetchContent_Declare(mcf5407
	GIT_REPOSITORY https://github.com/axiomantic/mcf5407.git
	GIT_TAG ${RG2_MCF5407_GIT_TAG}
)

add_subdirectory(rg2Lib)
add_subdirectory(rg2WineProxy)
add_subdirectory(rg2TestConsole)

if(${CMAKE_PROJECT_NAME}_BUILD_JUCEPLUGIN)
	add_subdirectory(rg2JucePlugin)
endif()
