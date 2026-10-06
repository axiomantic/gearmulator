# Source list for the repo track. Owned by the repo track.
#
# Append every source name this track adds under source/claudia/rg2/rg2Lib/ to
# RG2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

# ----------------- the ArtifactResolver interface

list(APPEND RG2LIB_SOURCES
	artifactResolver.h
	artifactResolver.cpp
)
