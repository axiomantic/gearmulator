cmake_minimum_required(VERSION 3.15)

add_subdirectory(g2Lib)
add_subdirectory(g2TestConsole)

if(${CMAKE_PROJECT_NAME}_BUILD_JUCEPLUGIN)
	add_subdirectory(g2JucePlugin)
endif()
