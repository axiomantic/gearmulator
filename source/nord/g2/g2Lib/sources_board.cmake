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

# ----------------- BRD-5, the anomaly log
#
# Header-only on purpose: BRD-5's Files: line names anomalyLog.h and no
# translation unit. Listed here for the IDE source group, like flash.h above.

list(APPEND G2LIB_SOURCES anomalyLog.h)

# ----------------- BRD-6, the C++ firmware extractor
list(APPEND G2LIB_SOURCES firmwareExtract.cpp)

# ----------------- BRD-7, the flash model
#
# flash.h declares the read-only flash surface and flash.cpp carries the
# implementation. The model is constructed with two (base, size) pairs the
# fixture supplies: no shipped header carries a chip-select base literal.

list(APPEND G2LIB_SOURCES
	flash.h
	flash.cpp
)

# ----------------- BRD-3, the two-tier interrupt controller
list(APPEND G2LIB_SOURCES
	interruptController.h
	interruptController.cpp
)
