# Source list for the dsp track. Owned by the dsp track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

list(APPEND G2LIB_SOURCES
	dspSet.h
	dspSet.cpp
)

# The translation unit is listed as well as the header: a build that compiles
# the test without compiling this source fails at the LINK step on
# g2::Hdi08Bridge.

list(APPEND G2LIB_SOURCES
	hdi08Bridge.h
	hdi08Bridge.cpp
)
