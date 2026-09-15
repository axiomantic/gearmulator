cmake_minimum_required(VERSION 3.15)

add_subdirectory(g2Lib)
add_subdirectory(g2TestConsole)

# This band carries g2JucePlugin/CMakeLists.txt, so the plugin is wired here and
# on no earlier band. Earlier bands hold a subset of the plugin's translation
# units, which g2Lib's test targets compile by path, and they removed this block
# because pointing CMake at a directory with no build file fails the configure.
# Merging one of those bands forward therefore conflicts on these lines: resolve
# it by keeping the add_subdirectory, not by taking the earlier band's removal.
if(${CMAKE_PROJECT_NAME}_BUILD_JUCEPLUGIN)
	add_subdirectory(g2JucePlugin)
endif()
