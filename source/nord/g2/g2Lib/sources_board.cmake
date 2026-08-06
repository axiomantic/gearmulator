# Source list for the board track. Owned by the board track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.
#
# Created empty by task BRD-0.

# ----------------- BRD-1, the memory decode and the two bus callbacks
list(APPEND G2LIB_SOURCES memoryMap.cpp)

# ----------------- BRD-2, the SIM registers
list(APPEND G2LIB_SOURCES sim.cpp)

# ----------------- BRD-12, the panel and the CS5 latches
list(APPEND G2LIB_SOURCES panel.cpp)
list(APPEND G2LIB_SOURCES latches.cpp)

# ----------------- BRD-15, the CS1 decode
list(APPEND G2LIB_SOURCES hdi08Decode.cpp)

# ----------------- BRD-6, the C++ firmware extractor
list(APPEND G2LIB_SOURCES firmwareExtract.cpp)
