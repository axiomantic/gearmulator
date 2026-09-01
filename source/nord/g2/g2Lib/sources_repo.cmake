# Source list for the repo track. Owned by the repo track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

# ----------------- the ArtifactResolver interface

list(APPEND G2LIB_SOURCES
	artifactResolver.h
	artifactResolver.cpp
)
