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
# translation unit. Listed here for the IDE source group, like flash.h below.

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

# ----------------- BRD-19, the bootstrap ROM
list(APPEND G2LIB_SOURCES
	hdi08Bootstrap.h
	hdi08Bootstrap.cpp
)

# ----------------- BRD-16, the HDI08 host-port adapter
list(APPEND G2LIB_SOURCES
	hdi08Adapter.h
	hdi08Adapter.cpp
)

# ----------------- BRD-4, UART0
#
# UART0 is the MCF5307 DUART module at MBAR+0x1C0 with vector 0x42 and the
# observed divider 0x36, 8N1. UART1 (MBAR+0x200) is unused and reads back its
# reset values; the same model owns both blocks. The transmitter buffer is the
# source for readMidiOut in the Device subclass (design section 14.5). The 54
# MHz clock once derived from the divider is refuted and must not return, so
# this model stores the divider as data and names no clock rate.

list(APPEND G2LIB_SOURCES
	uart0.h
	uart0.cpp
)

# ----------------- BRD-21, the Board class
#
# board.h declares the six-method surface the Scheduler uses -- runMcu,
# faulted, tickSofIfDue, stateSize, stateSave and stateLoad -- and carries the
# five static_asserts that make "concrete, not copyable, not movable" a
# COMPILE-TIME property. board.cpp carries the lifetime, the bodies and the one
# G2_MCU_CORE_CLOCK_HZ placeholder line that t0_board_surface counts.
#
# THE TRANSLATION UNIT IS LISTED HERE AND NOT ONLY THE HEADER. board.cpp
# defines every method t0_board_surface calls, so a build that compiles the
# test without compiling this source fails at the LINK step on g2::Board. BRD-22
# extends board.cpp with the real tickSofIfDue and adds no name to this list.

list(APPEND G2LIB_SOURCES
	board.h
	board.cpp
)
