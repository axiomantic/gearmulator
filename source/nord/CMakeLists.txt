cmake_minimum_required(VERSION 3.15)

if(${CMAKE_PROJECT_NAME}_SYNTH_NODALRED2X)
	add_subdirectory(n2x)
endif()

# Unconditional: it validates a file this branch carries, not a synth.
add_subdirectory(g2Tooling)
